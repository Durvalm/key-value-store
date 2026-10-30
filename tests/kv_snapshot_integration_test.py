"""Offline snapshot CLI against real stopped modulo and ring deployments."""
from contextlib import ExitStack
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from kv_layout_integration_test import make_groups, write_layout, owner_lookup, start_node
from kv_server_integration_test import start_server, stop_server, choose_port


def test_snapshot(server, client, snapshot_executable, ring):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        groups = make_groups(directory, 3 if ring else 2)
        layout = directory / 'source.layout'
        write_layout(layout, groups, positions=64 if ring else 1,
                     routing='fnv1a64-fmix64-ring-v1' if ring else 'fnv1a64-mod-v1')
        processes = []
        for index, group in enumerate(groups):
            for role in ['replica', 'primary']:
                if ring:
                    process = start_node(server, layout, group, role, cleanup)
                else:
                    process = start_server(server, group[f'{role}_log'], group[f'{role}_port'],
                        idle_timeout=10, role=role, history_id=group['history'], partition_id=index,
                        replica_port=group['replica_port'] if role == 'primary' else None)
                    cleanup.callback(stop_server, process)
                processes.append((role, process))
        quote = lambda value: json.dumps(value, ensure_ascii=False)
        commands = []
        values = {}
        # Enough keys to cover both layouts, with quoted/UTF-8 fields as well.
        keys = ['color', 'full name', 'quote"and\\slash', 'caf\u00e9'] + [f'snapshot-key-{i}' for i in range(20)]
        for key in keys:
            commands.append(f'SET {quote(key)} "old"')
            commands.append(f'SET {quote(key)} "latest"')
            values[key] = 'latest'
        commands.extend(['SET "deleted" "gone"', 'DELETE "deleted"',
                         'SET "recreated" "first"', 'DELETE "recreated"',
                         'SET "recreated" "second"', 'EXIT'])
        values['recreated'] = 'second'
        client_result = subprocess.run([str(client), '--layout', str(layout)],
            input='\n'.join(commands) + '\n', text=True, capture_output=True, timeout=15)
        assert client_result.returncode == 0, client_result.stderr
        assert client_result.stdout.splitlines() == ['OK'] * (len(keys)*2) + [
            'OK', 'INTEGER 1', 'OK', 'INTEGER 1', 'OK', 'BYE'], client_result.stdout
        original = {Path(group[f'{role}_log']): Path(group[f'{role}_log']).read_bytes()
                    for group in groups for role in ['primary', 'replica']}
        times = {path: path.stat().st_mtime_ns for path in original}
        output = directory / 'snapshot.txt'

        def capture(path=output, launcher=None):
            arguments = [str(snapshot_executable), str(layout), str(path)]
            if launcher is not None:
                arguments = [sys.executable, '-c', launcher, *arguments]
            return subprocess.run(arguments, text=True, capture_output=True, timeout=10)

        result = capture()
        assert result.returncode != 0 and 'Log is in use' in result.stderr, result.stderr
        assert not output.exists()
        # Reject a second process using an already owned log before it can replay/write.
        result = subprocess.run([str(server), groups[0]['primary_log'], str(choose_port())],
                                text=True, capture_output=True, timeout=3)
        assert result.returncode != 0 and 'Log is in use' in result.stderr, result.stderr
        for role, process in processes:
            if role == 'primary':
                stop_server(process)
        result = capture()
        assert result.returncode != 0 and 'Log is in use' in result.stderr, result.stderr
        assert not output.exists(), 'a running replica also prevents offline collection'
        for role, process in processes:
            if role == 'replica':
                stop_server(process)

        result = capture()
        assert result.returncode == 0, result.stderr
        lines = output.read_text().splitlines()
        assert lines[0].startswith('SNAPSHOT 1 ')
        data_start = lines.index('DATA')
        source_rows = [shlex.split(line) for line in lines[1:data_start]]
        assert len(source_rows) == len(groups)
        assert {row[1] for row in source_rows} == {group['id'] for group in groups}
        assert sum(int(row[4]) for row in source_rows) == len(values)
        entries = [shlex.split(line) for line in lines[data_start+1:-1]]
        assert all(row[0] == 'SET' and len(row) == 3 for row in entries)
        assert len(entries) == len(values) and {row[1]: row[2] for row in entries} == values
        assert [row[1] for row in entries] == sorted(values), 'export is deterministic by key'
        assert lines[-1] == f'END {len(values)}'
        for path, content in original.items():
            assert path.read_bytes() == content and path.stat().st_mtime_ns == times[path]

        result = capture()
        assert result.returncode != 0 and output.read_text().splitlines() == lines, 'existing output is preserved'
        result = capture(Path(groups[0]['primary_log']))
        assert result.returncode != 0
        assert Path(groups[0]['primary_log']).read_bytes() == original[Path(groups[0]['primary_log'])]
        failed_output = directory / 'failed-snapshot.txt'
        # Simulate a write failure without affecting source logs or this runner.
        launcher = '''
import os
import resource
import signal
import sys
signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
resource.setrlimit(resource.RLIMIT_FSIZE, (96, 96))
os.execv(sys.argv[1], sys.argv[1:])
'''
        result = capture(failed_output, launcher)
        assert result.returncode != 0 and 'writing snapshot' in result.stderr, result.stderr
        assert not failed_output.exists(), 'partial snapshot is removed on write failure'

        # A valid extra primary mutation models an uncertain write not yet caught up.
        if ring:
            owner = owner_lookup(groups)
            key = next(key for key in values if owner(key) == groups[0]['id'])
        else:
            key = 'color'  # Existing modulo owner is group-0.
        primary = Path(groups[0]['primary_log'])
        before = primary.read_bytes()
        sequence = len(before.splitlines()) - 1  # Two headers, then next sequence.
        extra = f'{sequence} SET {quote(key)} "unconfirmed"\n'.encode()
        primary.write_bytes(before + extra)
        result = capture(failed_output)
        assert result.returncode != 0 and 'not synchronized' in result.stderr, result.stderr
        assert not failed_output.exists() and primary.read_bytes() == before + extra
        primary.write_bytes(before)
        replica = Path(groups[-1]['replica_log'])
        before = replica.read_bytes()
        replica.unlink()
        result = capture(failed_output)
        assert result.returncode != 0 and 'missing or not a regular file' in result.stderr, result.stderr
        assert not failed_output.exists() and not replica.exists()
        replica.write_bytes(before)
        for path, content in original.items():
            assert path.read_bytes() == content


if __name__ == '__main__':
    server, client, snapshot = map(Path, sys.argv[1:])
    test_snapshot(server, client, snapshot, ring=False)
    test_snapshot(server, client, snapshot, ring=True)
    print('Offline snapshot process, preservation and failure checks passed.')
