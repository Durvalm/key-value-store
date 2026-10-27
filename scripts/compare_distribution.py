"""Run three paired GET/SET trials with fresh logs and save captured evidence."""
import argparse
from contextlib import ExitStack
from datetime import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from kv_server_integration_test import choose_port, start_server, stop_server


def command_output(arguments):
    return subprocess.check_output(arguments, text=True).strip()


def run_trial(build, groups, workload, trial):
    ports = []
    while len(ports) < groups * 2:
        port = choose_port()
        if port not in ports:
            ports.append(port)
    primary_ports, replica_ports = ports[:groups], ports[groups:]
    with tempfile.TemporaryDirectory(prefix='kv-distribution-bench-') as directory, ExitStack() as cleanup:
        primary_paths, replica_paths = [], []
        server_commands = []
        for group in range(groups):
            primary_path = Path(directory) / f'primary-{group}.log'
            replica_path = Path(directory) / f'replica-{group}.log'
            primary_paths.append(primary_path)
            replica_paths.append(replica_path)
            history_id = f'bench-{workload.lower()}-{groups}-{trial}-{group}'
            partition_id = group if groups == 2 else None
            for role, path, port in [('replica', replica_path, replica_ports[group]),
                                     ('primary', primary_path, primary_ports[group])]:
                server = start_server(build / 'kv_server', path, port, idle_timeout=120,
                    role=role, replica_port=replica_ports[group] if role == 'primary' else None,
                    history_id=history_id, partition_id=partition_id)
                cleanup.callback(stop_server, server)
                arguments = [str(build / 'kv_server'), str(path), str(port), '120', role]
                if role == 'primary':
                    arguments.append(str(replica_ports[group]))
                arguments.extend(['--history-id', history_id])
                if partition_id is not None:
                    arguments.extend(['--partition', str(partition_id)])
                server_commands.append(arguments)

        if groups == 1:
            arguments = [str(build / 'kv_benchmark'), str(primary_ports[0]), workload, '2']
        else:
            arguments = [str(build / 'kv_benchmark'), '--cluster', *map(str, primary_ports), workload]
        result = subprocess.run(arguments, text=True, capture_output=True, timeout=60, check=True)
        metrics = {}
        for line in result.stdout.splitlines():
            name, separator, value = line.partition(': ')
            if separator and name in ['Total duration (s)', 'Throughput (requests/s)',
                                     'Median latency (us)', 'p95 latency (us, nearest rank)']:
                metrics[name] = float(value)
        assert len(metrics) == 4, result.stdout
        assert 'Completed requests: 100000\n' in result.stdout, result.stdout
        for group in range(groups):
            assert f'Group {group} measured requests: {100000 // groups}\n' in result.stdout

        verification = None
        if workload == 'SET':
            verify_arguments = ([str(build / 'kv_benchmark'), str(replica_ports[0]), 'VERIFY']
                if groups == 1 else [str(build / 'kv_benchmark'), '--cluster',
                                    *map(str, replica_ports), 'VERIFY'])
            verification = subprocess.run(verify_arguments, text=True, capture_output=True,
                                          timeout=15, check=True).stdout
        checks = []
        for group in range(groups):
            history = primary_paths[group].read_bytes()
            assert history == replica_paths[group].read_bytes(), 'primary/replica history differs'
            lines = history.decode().splitlines()
            records = lines[1 if groups == 1 else 2:]
            expected_count = (100200 if workload == 'SET' else 100) // groups
            assert len(records) == expected_count, (len(records), expected_count)
            for sequence, line in enumerate(records, start=1):
                assert line.split(' ', 1)[0] == str(sequence), 'nonconsecutive history'
            keys = {line.split('"')[1] for line in records}
            assert len(keys) == 100 // groups
            checks.append({'group': group, 'measured_requests': 100000 // groups,
                'distinct_keys': len(keys), 'mutation_records': len(records),
                'matching_primary_replica_logs': True, 'log_bytes': len(history),
                'log_sha256': hashlib.sha256(history).hexdigest()})
        return {'trial': trial, 'groups': groups, 'workload': workload,
                'server_commands': server_commands, 'benchmark_command': arguments,
                'stdout': result.stdout, 'stderr': result.stderr, 'metrics': metrics,
                'replica_verification_stdout': verification, 'log_checks': checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-release')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in (build / 'CMakeCache.txt').read_text(), 'Use a Release build'
    evidence = {'recorded_at': datetime.now(ZoneInfo('America/New_York')).isoformat(),
        'base_commit': command_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD']),
        'working_tree_diff_stat': command_output(['git', '-C', str(ROOT), 'diff', '--stat']),
        'environment': {'os': platform.platform(), 'cpu': command_output(['sysctl', '-n', 'machdep.cpu.brand_string']),
            'logical_cpus': command_output(['sysctl', '-n', 'hw.logicalcpu']),
            'memory_bytes': command_output(['sysctl', '-n', 'hw.memsize']),
            'compiler': command_output(['c++', '--version']), 'cmake': command_output(['cmake', '--version'])},
        'protocol': 'Two closed-loop clients, persistent sockets, 100000 measured requests, 100 keys, 100 warm-up requests; 100 untimed preparation SETs; fresh processes/logs per run.',
        'runs': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for workload in ['GET', 'SET']:
        for trial in range(1, 4):
            # Alternate which configuration goes first to reduce order bias.
            for groups in ([1, 2] if trial % 2 else [2, 1]):
                run = run_trial(build, groups, workload, trial)
                evidence['runs'].append(run)
                args.output.write_text(json.dumps(evidence, indent=2) + '\n')
                print(f'{workload}, trial {trial}, {groups} group(s): '
                      f'{run["metrics"]["Throughput (requests/s)"]:.3f} requests/s', flush=True)
    print(f'Saved captured output and correctness checks to {args.output}', flush=True)


if __name__ == '__main__':
    main()
