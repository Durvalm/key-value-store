"""Exercise client routing against two independent standalone servers."""
import subprocess
import sys
import tempfile
from pathlib import Path

from kv_server_integration_test import (
    SERVER_HOST, choose_port, start_server, stop_server, open_client, expect_line,
)


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
        try:
            for index, port in enumerate(ports):
                servers.append(start_server(server_executable,
                    Path(directory) / f"partition-{index}.log", port, idle_timeout=5))

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
                        expect_line(stream, b'VALUE "second"\n' if index == owner else b'NOT_FOUND\n')

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
    print('Distribution routing tests passed.')


if __name__ == '__main__':
    main()
