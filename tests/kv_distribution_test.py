"""Exercise partition routing, ownership, and replicated failure isolation."""
import subprocess
import sys
import tempfile
from pathlib import Path

from kv_server_integration_test import (
    SERVER_HOST, choose_port, start_server, stop_server, open_client, expect_line,
)


def test_partitioned_replication(server_executable):
    # Check ownership and read-only behavior with one primary/replica pair.
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


def test_replicated_failure_isolation(server_executable, client_executable):
    # Four separate processes/logs; only group 0 is stopped and recovered.
    ports = []
    while len(ports) < 4:
        port = choose_port()
        if port not in ports:
            ports.append(port)
    primary_ports = ports[:2]
    replica_ports = ports[2:]
    arguments = [str(client_executable), '--cluster', SERVER_HOST,
                 *map(str, primary_ports)]

    def run(commands, expected):
        result = subprocess.run(arguments, input=commands, text=True,
                                capture_output=True, timeout=15)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == expected, result.stdout

    with tempfile.TemporaryDirectory() as directory:
        primary_paths = [Path(directory) / f'primary-{i}.log' for i in range(2)]
        replica_paths = [Path(directory) / f'replica-{i}.log' for i in range(2)]
        servers = []
        primaries = []
        replicas = []
        try:
            for i in range(2):
                replica = start_server(server_executable, replica_paths[i], replica_ports[i],
                    idle_timeout=10, role='replica', history_id=f'group-{i}', partition_id=i)
                servers.append(replica)
                replicas.append(replica)
                primary = start_server(server_executable, primary_paths[i], primary_ports[i],
                    idle_timeout=10, role='primary', replica_port=replica_ports[i],
                    history_id=f'group-{i}', partition_id=i)
                servers.append(primary)
                primaries.append(primary)

            run('SET "color" "blue"\nSET "full name" "Durval"\nEXIT\n',
                ['OK', 'OK', 'BYE'])
            for i in range(2):
                assert primary_paths[i].read_bytes() == replica_paths[i].read_bytes()
            replica_before = replica_paths[0].read_bytes()

            stop_server(replicas[0])
            # One session sees group 0 fail, still reads its locally applied
            # value, and continues writing/reading group 1. Later writes and
            # deletes on group 0 must be rejected before changing its log.
            run('SET "color" "yellow"\nSET "color" "orange"\nDELETE "color"\n'
                'GET "color"\nSET "full name" "Still working"\nGET "full name"\nEXIT\n',
                ['ERROR REPLICATION_UNCERTAIN "Write applied locally; replica acknowledgment is unconfirmed"',
                 'ERROR REPLICA_UNAVAILABLE "Replication failed; further writes are disabled"',
                 'ERROR REPLICA_UNAVAILABLE "Replication failed; further writes are disabled"',
                 'VALUE "yellow"', 'OK', 'VALUE "Still working"', 'BYE'])
            assert replica_paths[0].read_bytes() == replica_before
            expected_primary = replica_before + b'2 SET "color" "yellow"\n'
            assert primary_paths[0].read_bytes() == expected_primary
            assert primary_paths[1].read_bytes() == replica_paths[1].read_bytes()
            assert all(server.poll() is None for server in [*primaries, replicas[1]])

            stop_server(primaries[0])
            replica = start_server(server_executable, replica_paths[0], replica_ports[0],
                idle_timeout=10, role='replica', history_id='group-0', partition_id=0)
            servers.append(replica)
            primary = start_server(server_executable, primary_paths[0], primary_ports[0],
                idle_timeout=10, role='primary', replica_port=replica_ports[0],
                history_id='group-0', partition_id=0)
            servers.append(primary)

            # Reading the replica directly proves startup catch-up, rather than
            # merely proving that the primary replayed its own local log.
            with open_client(replica_ports[0]) as client, client.makefile('rb') as stream:
                client.sendall(b'GET "color"\n')
                expect_line(stream, b'VALUE "yellow"\n')
            assert primary_paths[0].read_bytes() == replica_paths[0].read_bytes() == expected_primary
            run('SET "color" "green"\nGET "color"\nGET "full name"\nEXIT\n',
                ['OK', 'VALUE "green"', 'VALUE "Still working"', 'BYE'])
            assert primary_paths[0].read_bytes() == replica_paths[0].read_bytes() == (
                expected_primary + b'3 SET "color" "green"\n')
            assert primary_paths[1].read_bytes() == replica_paths[1].read_bytes() == (
                b'PARTITION 1 fnv1a64-mod2-v1\nHISTORY group-1\n'
                b'1 SET "full name" "Durval"\n2 SET "full name" "Still working"\n')
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
    test_replicated_failure_isolation(server_executable, client_executable)
    print('Distribution routing, ownership, and failure-isolation tests passed.')


if __name__ == '__main__':
    main()
