"""Exercise client routing against two independent standalone servers."""
import subprocess
import sys
import tempfile
from pathlib import Path

from kv_server_integration_test import (
    SERVER_HOST, choose_port, start_server, stop_server, open_client, expect_line,
)


def test_partitioned_replication(server_executable):
    # Check the new option/header with an existing primary/replica pair. The
    # full two-group failure experiment remains the next milestone.
    with tempfile.TemporaryDirectory() as directory:
        primary_port, replica_port = choose_port(), choose_port()
        while primary_port == replica_port:
            replica_port = choose_port()
        primary_path = Path(directory) / 'primary.log'
        replica_path = Path(directory) / 'replica.log'
        servers = []
        try:
            servers.append(start_server(server_executable, replica_path, replica_port,
                idle_timeout=5, role='replica', history_id='group-1', partition_id=1))
            servers.append(start_server(server_executable, primary_path, primary_port,
                idle_timeout=5, role='primary', replica_port=replica_port,
                history_id='group-1', partition_id=1))
            with open_client(primary_port) as client, client.makefile('rb') as stream:
                client.sendall(b'SET "color" "wrong"\n')
                expect_line(stream, b'ERROR WRONG_PARTITION "Key belongs to another partition"\n')
                client.sendall(b'SET "full name" "Durval"\n')
                expect_line(stream, b'OK\n')
            with open_client(replica_port) as client, client.makefile('rb') as stream:
                client.sendall(b'GET "full name"\n')
                expect_line(stream, b'VALUE "Durval"\n')
                client.sendall(b'SET "full name" "wrong"\n')
                expect_line(stream, b'ERROR READ_ONLY "Client writes are disabled on this connection"\n')
            expected = ('PARTITION 1 fnv1a64-mod2-v1\nHISTORY group-1\n'
                        '1 SET "full name" "Durval"\n').encode()
            assert primary_path.read_bytes() == replica_path.read_bytes() == expected
        finally:
            for server in reversed(servers):
                stop_server(server)


def main():
    server_executable, client_executable = map(Path, sys.argv[1:])
    ports = [choose_port(), choose_port()]
    while ports[1] == ports[0]:
        ports[1] = choose_port()
    arguments = [str(client_executable), "--cluster", SERVER_HOST, *map(str, ports)]

    def run(commands):
        return subprocess.run(arguments, input=commands, text=True,
                              capture_output=True, timeout=15)

    with tempfile.TemporaryDirectory() as directory:
        servers = []
        for options in [['--partition', '2'], ['--partition', '-1'],
                        ['--partition', '0', '--partition', '1']]:
            path = Path(directory) / 'invalid-configuration.log'
            result = subprocess.run([str(server_executable), str(path), str(ports[0]), *options],
                                    capture_output=True, text=True, timeout=3)
            assert result.returncode != 0, result.stdout
            assert not path.exists(), 'invalid arguments must not create a log'
        try:
            for index, port in enumerate(ports):
                servers.append(start_server(server_executable,
                    Path(directory) / f"partition-{index}.log", port, idle_timeout=5, partition_id=index))

            # color belongs to partition 0, full name to partition 1. Exercise
            # every keyed operation, overwrites, and missing/deleted keys.
            commands = []
            expected = []
            for key in ['color', 'full name', 'caf\u00e9', 'a"b\\c']:
                escaped = key.replace('\\', '\\\\').replace('"', '\\"')
                commands.extend([f'SET "{escaped}" "first"', f'SET "{escaped}" "second"',
                                 f'GET "{escaped}"', f'EXISTS "{escaped}"'])
                expected.extend(['OK', 'OK', 'VALUE "second"', 'INTEGER 1'])
            # Malformed input and non-keyed commands must not reach either node.
            commands.extend(['SET unquoted "wrong"', 'SIZE', 'COMPACT', 'HELP'])
            result = run('\n'.join(commands) + '\nEXIT\n')
            assert result.returncode == 0, result.stderr
            lines = result.stdout.splitlines()
            assert lines[:len(expected)] == expected, result.stdout
            assert lines[len(expected)].startswith('ERROR INVALID_ARGUMENT'), result.stdout
            assert sum(line.startswith('ERROR UNSUPPORTED') for line in lines) == 2
            assert lines[-1] == 'BYE'

            # Direct reads prove the data lives only on the selected node.
            for index, port in enumerate(ports):
                with open_client(port) as client, client.makefile('rb') as stream:
                    for key, owner in [('color', 0), ('full name', 1), ('caf\u00e9', 1)]:
                        client.sendall(f'GET "{key}"\n'.encode())
                        expect_line(stream, b'VALUE "second"\n' if index == owner else
                                    b'ERROR WRONG_PARTITION "Key belongs to another partition"\n')

            # Wrong-node mutations must not append anything, and valid requests
            # on the same connection must continue to work afterward.
            for index, port in enumerate(ports):
                path = Path(directory) / f"partition-{index}.log"
                before = path.read_bytes()
                wrong_key = 'full name' if index == 0 else 'color'
                with open_client(port) as client, client.makefile('rb') as stream:
                    for command in [f'SET "{wrong_key}" "wrong"', f'DELETE "{wrong_key}"',
                                    f'EXISTS "{wrong_key}"']:
                        client.sendall((command + '\n').encode())
                        expect_line(stream, b'ERROR WRONG_PARTITION "Key belongs to another partition"\n')
                    client.sendall(b'HELP\n')
                    assert stream.readline().startswith(b'COMMANDS')
                assert path.read_bytes() == before

            # Restart with the same IDs and log assignments, then reject swapped
            # logs and omitted partition flags without modifying persisted bytes.
            for index, server in enumerate(servers):
                stop_server(server)
                path = Path(directory) / f"partition-{index}.log"
                before = path.read_bytes()
                for options in [[], ['--partition', str(1 - index)]]:
                    result = subprocess.run([str(server_executable), str(path), str(ports[index]), *options],
                                            capture_output=True, text=True, timeout=3)
                    assert result.returncode != 0, result.stdout
                    assert 'Partition log/configuration' in result.stderr, result.stderr
                    assert path.read_bytes() == before
                servers[index] = start_server(server_executable, path, ports[index],
                                              idle_timeout=5, partition_id=index)
            result = run('GET "color"\nGET "full name"\n')
            assert result.returncode == 0, result.stderr
            assert result.stdout.splitlines() == ['VALUE "second"', 'VALUE "second"'], result.stdout

            commands = []
            expected = []
            for key in ['color', 'full name']:
                commands.extend([f'DELETE "{key}"', f'DELETE "{key}"',
                                 f'GET "{key}"', f'EXISTS "{key}"'])
                expected.extend(['INTEGER 1', 'INTEGER 0', 'NOT_FOUND', 'INTEGER 0'])
            result = run('\n'.join(commands) + '\n')
            assert result.returncode == 0, result.stderr
            assert result.stdout.splitlines() == expected, result.stdout

            stop_server(servers[0])
            # An unavailable partition must not prevent the next request from
            # reaching the healthy partition in the same client session.
            result = run('GET "color"\nGET "caf\u00e9"\nEXIT\n')
            assert result.returncode != 0
            assert result.stdout.splitlines() == [
                'ERROR PARTITION_UNAVAILABLE "Could not connect to partition 0"',
                'VALUE "second"', 'BYE'], result.stdout
        finally:
            for server in servers:
                stop_server(server)

    # Local commands and invalid requests do not require a running server.
    result = run('GET unquoted\nHELP\nEXIT\n')
    assert result.returncode == 0, result.stderr
    assert result.stdout.startswith('ERROR INVALID_ARGUMENT'), result.stdout
    assert result.stdout.endswith('BYE\n')
    test_partitioned_replication(server_executable)
    print('Distribution routing and ownership tests passed.')


if __name__ == '__main__':
    main()
