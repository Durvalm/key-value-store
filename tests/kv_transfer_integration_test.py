"""Offline transfer through real replicated destinations; no activation."""
from contextlib import ExitStack
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

from kv_layout_integration_test import make_groups, write_layout, owner_lookup, start_node, request
from kv_server_integration_test import stop_server


def seed_source(server, client, layout, groups, cleanup):
    processes = []
    for group in groups:
        processes.append(start_node(server, layout, group, 'replica', cleanup))
        processes.append(start_node(server, layout, group, 'primary', cleanup))
    quote = lambda text: json.dumps(text, ensure_ascii=False)
    values = {key: f'value {i} "quoted" \\' for i, key in enumerate(
        ['color', 'full name', 'quote"key', 'caf\u00e9'] + [f'transfer-key-{i}' for i in range(60)])}
    commands = [f'SET {quote(key)} {quote(value)}' for key, value in values.items()]
    commands.extend(['SET "color" "latest"', 'SET "deleted" "gone"', 'DELETE "deleted"',
                     'SET "recreated" "before"', 'DELETE "recreated"', 'SET "recreated" "after"', 'EXIT'])
    result = subprocess.run([str(client), '--layout', str(layout)], input='\n'.join(commands)+'\n',
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ['OK']*len(values) + [
        'OK', 'OK', 'INTEGER 1', 'OK', 'INTEGER 1', 'OK', 'BYE'], result.stdout
    values['color'] = 'latest'
    values['recreated'] = 'after'
    for process in reversed(processes):
        stop_server(process)
    return values


def test_transfer(server, client, transfer, verify, activate, source_ring):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        old_dir, new_dir = directory/'old', directory/'new'
        old_dir.mkdir()
        new_dir.mkdir()
        sources = make_groups(old_dir, 3 if source_ring else 2)
        source_layout = directory/'old.layout'
        write_layout(source_layout, sources, positions=64 if source_ring else 1,
                     routing='fnv1a64-fmix64-ring-v1' if source_ring else 'fnv1a64-mod-v1')
        values = seed_source(server, client, source_layout, sources, cleanup)
        original = {Path(group[f'{role}_log']): (Path(group[f'{role}_log']).read_bytes(),
                    Path(group[f'{role}_log']).stat().st_mtime_ns)
                    for group in sources for role in ['primary', 'replica']}
        targets = make_groups(new_dir, len(sources)+1)
        for i, group in enumerate(targets):
            group['history'] = f'new-history-{i}'
        target_layout = directory/'new.layout'
        write_layout(target_layout, targets)
        processes = []
        for group in targets:
            processes.append(start_node(server, target_layout, group, 'replica', cleanup))
            processes.append(start_node(server, target_layout, group, 'primary', cleanup))
        owner = owner_lookup(targets)
        counts = {group['id']: sum(owner(key) == group['id'] for key in values) for group in targets}
        assert all(counts.values()), counts

        # Bad control requests must not claim the fresh destination or mutate it.
        first = targets[0]
        identity = Path(first['primary_log']).read_text().splitlines()[0]
        request(first['primary_port'], f'IMPORT wrong {json.dumps(identity)}', 'ERROR IMPORT_IDENTITY_MISMATCH')
        request(first['primary_port'], f'IMPORT {first["history"]} unquoted', 'ERROR INVALID_IMPORT')
        saved_configs = [source_layout.read_bytes(), target_layout.read_bytes()]
        for field, value, error in [('history', sources[0]['history'], 'history IDs must be new'),
                                    ('primary_log', sources[0]['primary_log'], 'must not overlap')]:
            invalid = [dict(group) for group in targets]
            invalid[0][field] = value
            bad_layout = directory/'invalid.layout'
            write_layout(bad_layout, invalid)
            rejected = subprocess.run([str(transfer), str(source_layout), str(bad_layout)],
                                      text=True, capture_output=True, timeout=10)
            assert rejected.returncode != 0 and error in rejected.stderr, rejected.stderr
        # A later group's preflight rejection must occur before any earlier
        # group receives a SET. Empty reserved processes can be restarted safely.
        last = targets[-1]
        last_identity = Path(last['primary_log']).read_text().splitlines()[0]
        request(last['primary_port'], f'IMPORT {last["history"]} {json.dumps(last_identity)}', 'READY_IMPORT')
        rejected = subprocess.run([str(transfer), str(source_layout), str(target_layout)],
                                  text=True, capture_output=True, timeout=10)
        assert rejected.returncode != 0 and 'IMPORT_SESSION_USED' in rejected.stderr, rejected.stderr
        for group in targets:
            assert len(Path(group['primary_log']).read_text().splitlines()) == 2
            assert Path(group['primary_log']).read_bytes() == Path(group['replica_log']).read_bytes()
        for process in reversed(processes):
            stop_server(process)
        processes = []
        for group in targets:
            processes.append(start_node(server, target_layout, group, 'replica', cleanup))
            processes.append(start_node(server, target_layout, group, 'primary', cleanup))
        result = subprocess.run([str(transfer), str(source_layout), str(target_layout)],
                                text=True, capture_output=True, timeout=20)
        assert result.returncode == 0, result.stderr
        assert f'Transferred {len(values)} live keys with replica acknowledgments.' in result.stdout
        for group in targets:
            assert f'{group["id"]}: {counts[group["id"]]} acknowledged SETs' in result.stdout
            primary = Path(group['primary_log']).read_bytes()
            assert primary == Path(group['replica_log']).read_bytes()
            lines = primary.decode().splitlines()
            assert lines[1] == 'HISTORY ' + group['history']
            assert len(lines)-2 == counts[group['id']]
            assert [int(line.split(' ',1)[0]) for line in lines[2:]] == list(range(1, counts[group['id']]+1))
            for role in ['primary', 'replica']:
                for key, value in values.items():
                    if owner(key) == group['id']:
                        request(group[f'{role}_port'], f'GET {json.dumps(key, ensure_ascii=False)}',
                                'VALUE ' + json.dumps(value, ensure_ascii=False))
                request(group[f'{role}_port'], 'SIZE', f'INTEGER {counts[group["id"]]}')
            key = next(key for key in values if owner(key) == group['id'])
            request(group['primary_port'], f'SET {json.dumps(key)} "accidental"',
                    'ERROR IMPORT_IN_PROGRESS "Destination is reserved for import"')
        deleted_owner = next(group for group in targets if group['id'] == owner('deleted'))
        request(deleted_owner['replica_port'], 'GET "deleted"', 'NOT_FOUND')
        for path, (contents, timestamp) in original.items():
            assert path.read_bytes() == contents and path.stat().st_mtime_ns == timestamp
        assert [source_layout.read_bytes(), target_layout.read_bytes()] == saved_configs

        checked = subprocess.run([str(verify), str(source_layout), str(target_layout)],
                                 text=True, capture_output=True, timeout=20)
        assert checked.returncode == 0, checked.stderr
        assert f'Verified {len(values)} live keys' in checked.stdout

        active = directory/'active.layout'
        active.write_bytes(source_layout.read_bytes())
        rejected = subprocess.run([str(activate), str(source_layout), str(target_layout), str(active)],
                                  text=True, capture_output=True, timeout=20)
        assert rejected.returncode != 0 and 'ACTIVATION_NOT_READY' in rejected.stderr, rejected.stderr
        assert active.read_bytes() == source_layout.read_bytes()

        saved_targets = {Path(group[f'{role}_log']): Path(group[f'{role}_log']).read_bytes()
                         for group in targets for role in ['primary','replica']}
        retry = subprocess.run([str(transfer), str(source_layout), str(target_layout)],
                               text=True, capture_output=True, timeout=10)
        assert retry.returncode != 0 and 'fresh header-only' in retry.stderr, retry.stderr
        assert all(path.read_bytes() == content for path, content in saved_targets.items())
        for process in reversed(processes):
            stop_server(process)
        for group in targets:
            start_node(server, target_layout, group, 'replica', cleanup)
            start_node(server, target_layout, group, 'primary', cleanup)
            for key, value in values.items():
                if owner(key) == group['id']:
                    request(group['replica_port'], f'GET {json.dumps(key, ensure_ascii=False)}',
                            'VALUE '+json.dumps(value, ensure_ascii=False))
            assert Path(group['primary_log']).read_bytes() == Path(group['replica_log']).read_bytes()
        checked = subprocess.run([str(verify), str(source_layout), str(target_layout)],
                                 text=True, capture_output=True, timeout=20)
        assert checked.returncode == 0, checked.stderr
        assert all(path.read_bytes() == content for path, content in saved_targets.items())
        activated = subprocess.run([str(activate), str(source_layout), str(target_layout), str(active)],
                                   text=True, capture_output=True, timeout=20)
        assert activated.returncode == 0 and 'published active layout' in activated.stdout, activated.stderr
        # Clients load the published file, then ordinary replicated writes work.
        result = subprocess.run([str(client), '--layout', str(active)],
            input='GET "color"\nGET "deleted"\nSET "after-activation" "works"\nEXIT\n',
            text=True, capture_output=True, timeout=15)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == ['VALUE "latest"', 'NOT_FOUND', 'OK', 'BYE'], result.stdout
        owner_group = next(group for group in targets if group['id'] == owner('after-activation'))
        request(owner_group['replica_port'], 'GET "after-activation"', 'VALUE "works"')
        for path, (contents, timestamp) in original.items():
            assert path.read_bytes() == contents and path.stat().st_mtime_ns == timestamp
        assert [source_layout.read_bytes(), target_layout.read_bytes()] == saved_configs


def test_partial_failure(server, client, transfer, snapshot):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        (directory/'old').mkdir()
        (directory/'new').mkdir()
        sources = make_groups(directory/'old', 1)
        old = directory/'old.layout'
        write_layout(old, sources, positions=1, routing='fnv1a64-mod-v1')
        seed_source(server, client, old, sources, cleanup)
        original = {Path(group[f'{role}_log']): Path(group[f'{role}_log']).read_bytes()
                    for group in sources for role in ['primary','replica']}
        targets = make_groups(directory/'new', 1)
        target = targets[0]
        target['history'] = 'new-history'
        actual = directory/'actual.layout'
        write_layout(actual, targets)
        replica = start_node(server, actual, target, 'replica', cleanup)
        start_node(server, actual, target, 'primary', cleanup)
        # Proxy only controls timing: actual destination code still handles
        # IMPORT, normal SETs and real replication failure.
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(8)
            proposed = [dict(target, primary_port=listener.getsockname()[1])]
            candidate = directory/'candidate.layout'
            write_layout(candidate, proposed)
            seen, errors = [], []

            def proxy():
                try:
                    connection, _ = listener.accept()
                    with connection, socket.create_connection(('127.0.0.1', target['primary_port']), timeout=5) as upstream:
                        connection.settimeout(8)
                        upstream.settimeout(8)
                        with connection.makefile('rb') as incoming, upstream.makefile('rb') as replies:
                            for line in incoming:
                                upstream.sendall(line)
                                response = replies.readline()
                                if line.startswith(b'SET '):
                                    seen.append(line)
                                    if len(seen) == 1:
                                        assert response == b'OK\n', response
                                        # Capture has finished, but transfer must still
                                        # hold its source leases while awaiting replies.
                                        attempt = subprocess.run([str(server), '--layout', str(old), sources[0]['id'], 'replica'],
                                            text=True, capture_output=True, timeout=3)
                                        assert attempt.returncode != 0 and 'Log is in use' in attempt.stderr, attempt.stderr
                                        stop_server(replica)
                                connection.sendall(response)
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=proxy)
            thread.start()
            try:
                result = subprocess.run([str(transfer), str(old), str(candidate)],
                                        text=True, capture_output=True, timeout=20)
            finally:
                thread.join(timeout=10)
            assert not thread.is_alive() and not errors, errors
            assert result.returncode != 0 and 'REPLICATION_UNCERTAIN' in result.stderr, result.stderr
            assert len(seen) == 2, 'no retry or later write after uncertain acknowledgment'
            primary_records = Path(target['primary_log']).read_text().splitlines()[2:]
            replica_records = Path(target['replica_log']).read_text().splitlines()[2:]
            assert len(primary_records) == 2 and len(replica_records) == 1
            assert all(path.read_bytes() == content for path, content in original.items())
            assert 'Transferred' not in result.stdout
            # Failure must close sockets and release all source locks.
            check = subprocess.run([str(snapshot), str(old), str(directory/'after.snapshot')],
                                   text=True, capture_output=True, timeout=10)
            assert check.returncode == 0, check.stderr


if __name__ == '__main__':
    server, client, transfer, snapshot, verify, activate = map(Path, sys.argv[1:])
    test_transfer(server, client, transfer, verify, activate, source_ring=False)
    test_transfer(server, client, transfer, verify, activate, source_ring=True)
    test_partial_failure(server, client, transfer, snapshot)
    print('Destination transfer, source preservation and uncertainty checks passed.')
