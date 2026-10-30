"""Exact verification against real nodes, including rejection and read-only checks."""
from contextlib import ExitStack
import json
from pathlib import Path
import subprocess
import socket
import threading
import sys
import tempfile

from kv_layout_integration_test import make_groups, write_layout, start_node, request


def quote(text):
    return json.dumps(text, ensure_ascii=False)


def save_logs(group, values):
    lines = [f'OWNERSHIP {quote(group["id"])} fnv1a64-fmix64-ring-v1 64 {quote(group["id"])}',
             'HISTORY ' + group['history']]
    lines += [f'{i} SET {quote(key)} {quote(value)}'
              for i, (key, value) in enumerate(values.items(), 1)]
    for role in ['primary', 'replica']:
        Path(group[f'{role}_log']).write_text('\n'.join(lines) + '\n')


def test_case(server, verify, change):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        (directory/'old').mkdir()
        (directory/'new').mkdir()
        source = make_groups(directory/'old', 1)[0]
        target = make_groups(directory/'new', 1)[0]
        target['history'] = 'new-history'
        old, new = directory/'old.layout', directory/'new.layout'
        write_layout(old, [source])
        write_layout(new, [target])
        # Includes a reply above the ACK helper's original 512-byte bound.
        expected = {'café "key': 'x'*1500 + ' "quoted" \\', 'second': 'latest'}
        if change == 'empty':
            expected = {}
        actual = dict(expected)
        if change == 'value':
            actual['second'] = 'wrong'
        elif change == 'missing':
            del actual['second']
        elif change == 'extra':
            actual['unwanted'] = 'leftover'
        save_logs(source, expected)
        save_logs(target, actual)
        start_node(server, new, target, 'replica', cleanup)
        start_node(server, new, target, 'primary', cleanup)
        request(target['primary_port'], 'VERIFY unquoted identity node', 'ERROR INVALID_VERIFY')
        request(target['replica_port'], 'VERIFY wrong "ownership" "node" extra', 'ERROR INVALID_VERIFY')
        candidate = dict(target)
        if change == 'history':
            candidate['history'] = 'wrong-history'
        elif change == 'path':
            candidate['primary_log'] = str(directory/'wrong.log')
        elif change == 'role':
            candidate['primary_port'], candidate['replica_port'] = candidate['replica_port'], candidate['primary_port']
        write_layout(new, [candidate], positions=32 if change == 'layout' else 64)
        paths = [old, new] + [Path(g[f'{r}_log']) for g in [source, target] for r in ['primary', 'replica']]
        before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths}
        result = subprocess.run([str(verify), str(old), str(new)], text=True, capture_output=True, timeout=15)
        if change in ['correct', 'empty']:
            assert result.returncode == 0, result.stderr
            assert f'Verified {len(expected)} live keys' in result.stdout
        else:
            assert result.returncode != 0 and 'Verified' not in result.stdout, result.stdout
            error = ('Value mismatch' if change in ['value', 'missing'] else
                     'Key count mismatch' if change == 'extra' else 'VERIFY_IDENTITY_MISMATCH')
            assert error in result.stderr, result.stderr
        assert all((p.read_bytes(), p.stat().st_mtime_ns) == saved for p, saved in before.items())


def test_peer_checks(verify, mode):
    # Deterministic protocol peers expose states that a synchronized real pair
    # cannot conveniently produce: replica-only corruption or a mid-check write.
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        (directory/'old').mkdir()
        (directory/'new').mkdir()
        source = make_groups(directory/'old', 1)[0]
        target = make_groups(directory/'new', 1)[0]
        target['history'] = 'new-history'
        old, new = directory/'old.layout', directory/'new.layout'
        write_layout(old, [source])
        write_layout(new, [target])
        save_logs(source, {'key': 'value'})
        threads, errors = [], []

        def serve(listener, role):
            try:
                connection, _ = listener.accept()
                with connection, connection.makefile('rb') as stream:
                    connection.settimeout(5)
                    checks = 0
                    for line in stream:
                        if line.startswith(b'VERIFY '):
                            checks += 1
                            progress = '1'
                            if role == 'replica':
                                if mode == 'progress' or (mode == 'changed' and checks == 2):
                                    progress = '2'
                                elif mode == 'malformed':
                                    progress = '-1'
                            response = 'VERIFY_READY ' + progress
                        elif line.startswith(b'GET '):
                            response = 'VALUE "wrong"' if role == 'replica' and mode == 'value' else 'VALUE "value"'
                        else:
                            assert line == b'SIZE\n', line
                            response = 'INTEGER 2' if role == 'replica' and mode == 'count' else 'INTEGER 1'
                        connection.sendall((response + '\n').encode())
            except BaseException as error:
                errors.append(error)

        for role in ['primary', 'replica']:
            listener = cleanup.enter_context(socket.socket())
            listener.bind(('127.0.0.1', target[f'{role}_port']))
            listener.listen(1)
            listener.settimeout(5)
            thread = threading.Thread(target=serve, args=(listener, role))
            thread.start()
            threads.append(thread)
        try:
            result = subprocess.run([str(verify), str(old), str(new)], text=True, capture_output=True, timeout=15)
        finally:
            for thread in threads:
                thread.join(timeout=6)
        assert not errors and all(not thread.is_alive() for thread in threads), errors
        expected = {'value': 'Value mismatch on group-0/replica',
                    'count': 'Key count mismatch on group-0/replica',
                    'changed': 'Destination changed during verification',
                    'progress': 'Primary/replica progress differs',
                    'malformed': 'Invalid verification progress'}[mode]
        assert result.returncode != 0 and expected in result.stderr, result.stderr


if __name__ == '__main__':
    server, verify = map(Path, sys.argv[1:])
    for case in ['correct', 'empty', 'value', 'missing', 'extra', 'history', 'layout', 'path', 'role']:
        test_case(server, verify, case)
    for mode in ['value', 'count', 'changed', 'progress', 'malformed']:
        test_peer_checks(verify, mode)
    print('Exact values, counts, identity, large replies and read-only checks passed.')
