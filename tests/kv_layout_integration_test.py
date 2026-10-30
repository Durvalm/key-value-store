"""Layout-driven routing, ownership, replication, and saved-identity checks."""
from bisect import bisect_left
from contextlib import ExitStack
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from kv_server_integration_test import choose_port, start_server, stop_server, open_client, expect_line


def make_groups(directory, count, long_ids=False):
    ports = []
    while len(ports) < count * 2:
        port = choose_port()
        if port not in ports:
            ports.append(port)
    return [dict(id=('group-' + ('x' * 105 if long_ids else '') + str(i)),
                 history=f'history-{i}', primary_port=ports[2*i], replica_port=ports[2*i+1],
                 primary_log=str(directory / f'primary-{i}.log'),
                 replica_log=str(directory / f'replica-{i}.log')) for i in range(count)]


def write_layout(path, groups, positions=64, routing="fnv1a64-fmix64-ring-v1"):
    lines = [f'LAYOUT 1 {routing} {positions}']
    for group in groups:
        fields = ['GROUP', json.dumps(group['id']), json.dumps(group['history']),
                  str(group['primary_port']), str(group['replica_port']),
                  json.dumps(group['primary_log']), json.dumps(group['replica_log'])]
        lines.append(' '.join(fields))
    path.write_text('\n'.join(lines) + '\n')


def owner_lookup(groups, positions=64):
    # Independent integer reference; fixed C++ hash vectors test the same rule.
    def hash_text(text):
        mask = (1 << 64) - 1
        value = 14695981039346656037
        for byte in text.encode():
            value = ((value ^ byte) * 1099511628211) & mask
        value ^= value >> 33
        value = value * 0xff51afd7ed558ccd & mask
        value ^= value >> 33
        value = value * 0xc4ceb9fe1a85ec53 & mask
        return value ^ (value >> 33)
    ring = sorted((hash_text(group['id'] + (f'#{i}' if i else '')), group['id'])
                  for group in groups for i in range(positions))
    points = [position for position, group in ring]
    return lambda key: ring[bisect_left(points, hash_text(key)) % len(ring)][1]


def start_node(executable, layout, group, role, cleanup):
    process = start_server(executable, group[f'{role}_log'], group[f'{role}_port'],
        idle_timeout=10, role=role, layout_path=layout, group_id=group['id'])
    cleanup.callback(stop_server, process)
    return process


def request(port, command, expected):
    with open_client(port) as client, client.makefile('rb') as stream:
        client.sendall((command + '\n').encode())
        expect_line(stream, expected.encode() + b'\n')


def test_three_groups(server, client):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        groups = make_groups(directory, 3)
        layout = directory / 'ring.layout'
        write_layout(layout, groups)
        owner = owner_lookup(groups)
        processes = []
        for group in groups:
            processes.append(start_node(server, layout, group, 'replica', cleanup))
            processes.append(start_node(server, layout, group, 'primary', cleanup))

        def run(commands, expected):
            result = subprocess.run([str(client), '--layout', str(layout)], input=commands,
                                    text=True, capture_output=True, timeout=15)
            assert result.returncode == 0, result.stderr
            assert result.stdout.splitlines() == expected, result.stdout

        keys = ['color', 'full name', 'quote"key', 'caf\u00e9'] + [f'layout-key-{i}' for i in range(96)]
        quote = lambda text: json.dumps(text, ensure_ascii=False)
        values = {key: f'value-{i}' for i, key in enumerate(keys)}
        commands = ''.join(f'SET {quote(key)} {quote(value)}\n' for key, value in values.items())
        commands += 'SET "color" "updated"\nDELETE "full name"\nGET "full name"\n'
        commands += 'EXISTS "full name"\nSIZE\nCOMPACT\nEXIT\n'
        run(commands, ['OK'] * 100 + ['OK', 'INTEGER 1', 'NOT_FOUND', 'INTEGER 0',
            'ERROR UNSUPPORTED "SIZE and COMPACT are unsupported in cluster mode"',
            'ERROR UNSUPPORTED "SIZE and COMPACT are unsupported in cluster mode"', 'BYE'])
        values['color'] = 'updated'
        del values['full name']
        assert {owner(key) for key in values} == {group['id'] for group in groups}
        for group in groups:
            for role in ['primary', 'replica']:
                for key, value in values.items():
                    if owner(key) == group['id']:
                        request(group[f'{role}_port'], f'GET {quote(key)}', f'VALUE {quote(value)}')
                wrong = next(key for key in values if owner(key) != group['id'])
                before = Path(group[f'{role}_log']).read_bytes()
                for command in [f'GET {quote(wrong)}', f'SET {quote(wrong)} "wrong"', f'DELETE {quote(wrong)}']:
                    request(group[f'{role}_port'], command, 'ERROR WRONG_PARTITION "Key belongs to another partition"')
                assert Path(group[f'{role}_log']).read_bytes() == before
            owned = next(key for key in values if owner(key) == group['id'])
            request(group['replica_port'], f'SET {quote(owned)} "wrong"',
                    'ERROR READ_ONLY "Client writes are disabled on this connection"')
            primary = Path(group['primary_log']).read_bytes()
            assert primary == Path(group['replica_log']).read_bytes()
            lines = primary.decode().splitlines()
            assert lines[0].startswith(f'OWNERSHIP "{group["id"]}" fnv1a64-fmix64-ring-v1 64 ')
            assert lines[1] == 'HISTORY ' + group['history']
            assert [int(line.split(' ', 1)[0]) for line in lines[2:]] == list(range(1, len(lines)-1))

        saved = {group['id']: Path(group['primary_log']).read_bytes() for group in groups}
        for process in reversed(processes):
            stop_server(process)
        changed = directory / 'changed.layout'
        write_layout(changed, groups, positions=32)
        for group in groups:
            result = subprocess.run([str(server), '--layout', str(changed), group['id'], 'replica'],
                                    text=True, capture_output=True, timeout=3)
            assert result.returncode != 0
            assert 'Saved ownership layout does not match' in result.stderr, result.stderr
            assert Path(group['replica_log']).read_bytes() == saved[group['id']]
        # Ring configuration order is irrelevant; canonical log identity must agree.
        write_layout(layout, list(reversed(groups)))
        for group in groups:
            start_node(server, layout, group, 'replica', cleanup)
            start_node(server, layout, group, 'primary', cleanup)
        run(''.join(f'GET {quote(key)}\n' for key in values) + 'EXIT\n',
            [f'VALUE {quote(value)}' for value in values.values()] + ['BYE'])
        for group in groups:
            assert Path(group['primary_log']).read_bytes() == Path(group['replica_log']).read_bytes() == saved[group['id']]
        run('SET "color" "after-restart"\nEXIT\n', ['OK', 'BYE'])
        for group in groups:
            assert Path(group['primary_log']).read_bytes() == Path(group['replica_log']).read_bytes()
        destination = next(group for group in groups if group['id'] == owner('color'))
        request(destination['replica_port'], 'GET "color"', 'VALUE "after-restart"')


def test_legacy_modulo_layout(server, client):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        groups = make_groups(directory, 2)
        processes = []
        for index, group in enumerate(groups):
            for role in ['replica', 'primary']:
                process = start_server(server, group[f'{role}_log'], group[f'{role}_port'],
                    idle_timeout=10, role=role,
                    replica_port=group['replica_port'] if role == 'primary' else None,
                    history_id=group['history'], partition_id=index)
                processes.append(process)
                cleanup.callback(stop_server, process)
        result = subprocess.run([str(client), '--cluster', '127.0.0.1',
            str(groups[0]['primary_port']), str(groups[1]['primary_port'])],
            input='SET "color" "blue"\nSET "full name" "Durval"\nEXIT\n',
            text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == ['OK', 'OK', 'BYE'], result.stdout
        for process in reversed(processes):
            stop_server(process)
        saved = [Path(group['primary_log']).read_bytes() for group in groups]
        layout = directory / 'modulo.layout'
        write_layout(layout, groups, positions=1, routing='fnv1a64-mod-v1')
        for group in groups:
            start_node(server, layout, group, 'replica', cleanup)
            start_node(server, layout, group, 'primary', cleanup)
        result = subprocess.run([str(client), '--layout', str(layout)],
            input='GET "color"\nGET "full name"\nEXIT\n',
            text=True, capture_output=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == ['VALUE "blue"', 'VALUE "Durval"', 'BYE'], result.stdout
        for index, group in enumerate(groups):
            assert Path(group['primary_log']).read_bytes() == Path(group['replica_log']).read_bytes() == saved[index]
            assert saved[index].startswith(f'PARTITION {index} fnv1a64-mod2-v1\n'.encode())


def test_peer_layout_mismatch(server):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        # Exercise a valid ownership header larger than the ordinary 4096-byte
        # request limit, including fragmented TCP reads during the handshake.
        groups = make_groups(directory, 40, long_ids=True)
        correct = directory / 'correct.layout'
        wrong = directory / 'wrong.layout'
        write_layout(correct, groups)
        wrong_groups = [dict(group) for group in groups]
        wrong_groups[0]['primary_log'] = str(directory / 'wrong-primary.log')
        write_layout(wrong, wrong_groups, positions=32)
        group = groups[0]
        start_node(server, correct, group, 'replica', cleanup)
        result = subprocess.run([str(server), '--layout', str(wrong), group['id'], 'primary', '10'],
                                text=True, capture_output=True, timeout=10)
        assert result.returncode != 0, result.stdout
        assert 'ERROR LAYOUT_MISMATCH' in result.stderr, result.stderr
        assert len(Path(group['replica_log']).read_text().splitlines()[0]) > 4096
        # A rejected mismatch must not consume the replica's one peer session.
        start_node(server, correct, group, 'primary', cleanup)
        identity = Path(group['primary_log']).read_text().splitlines()[0]
        request(group['primary_port'], 'IMPORT wrong ' + json.dumps(identity), 'ERROR IMPORT_IDENTITY_MISMATCH')
        owner = owner_lookup(groups)
        key = next(f'probe-{i}' for i in range(10000) if owner(f'probe-{i}') == group['id'])
        request(group['primary_port'], f'SET "{key}" "works"', 'OK')
        request(group['replica_port'], f'GET "{key}"', 'VALUE "works"')
        assert Path(group['primary_log']).read_bytes() == Path(group['replica_log']).read_bytes()


if __name__ == '__main__':
    server, client = map(Path, sys.argv[1:])
    test_three_groups(server, client)
    test_peer_layout_mismatch(server)
    test_legacy_modulo_layout(server, client)
    print('Layout routing, ownership, replication and restart checks passed.')
