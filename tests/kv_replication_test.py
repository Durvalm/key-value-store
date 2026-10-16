"""First-session forwarding tests; numbered replay/catch-up is not implemented."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

from kv_server_integration_test import choose_port, open_client, start_server, stop_server


def request(port, command):
    with open_client(port) as client, client.makefile("rb") as stream:
        client.sendall(command + b"\n")
        return stream.readline()


def distinct_ports():
    first = choose_port()
    second = choose_port()
    while second == first:
        second = choose_port()
    return first, second


def test_pair(executable, directory):
    primary_port, replica_port = distinct_ports()
    primary_log = directory / "primary.log"
    replica_log = directory / "replica.log"
    replica = start_server(executable, replica_log, replica_port, 30, "replica")
    primary = None
    try:
        primary = start_server(executable, primary_log, primary_port, 30, "primary", replica_port)
        assert request(replica_port, b'SET "direct" "no"').startswith(b"ERROR READ_ONLY")
        assert request(primary_port, b"COMPACT").startswith(b"ERROR COMPACTION_DISABLED")
        assert request(replica_port, b"COMPACT").startswith(b"ERROR COMPACTION_DISABLED")
        assert request(primary_port, b'SET "missing-value"').startswith(b"ERROR INVALID_ARGUMENT")
        assert request(primary_port, b'GET "absent"') == b"NOT_FOUND\n"
        assert request(primary_port, b'DELETE "absent"') == b"INTEGER 0\n"
        assert primary_log.read_bytes() == replica_log.read_bytes() == b""

        assert request(primary_port, b'SET "name" "Durval"') == b"OK\n"
        assert request(replica_port, b'GET "name"') == b'VALUE "Durval"\n'
        assert request(primary_port, b'SET "name" "new value"') == b"OK\n"
        assert request(replica_port, b'GET "name"') == b'VALUE "new value"\n'
        assert request(primary_port, b'DELETE "name"') == b"INTEGER 1\n"
        assert request(replica_port, b'GET "name"') == b"NOT_FOUND\n"
        assert len(primary_log.read_text().splitlines()) == 3
        assert primary_log.read_bytes() == replica_log.read_bytes()

        # Two workers write one shared key: both logs must preserve identical order.
        def writer(prefix):
            with open_client(primary_port) as client, client.makefile("rb") as stream:
                for i in range(50):
                    client.sendall(f'SET "shared" "{prefix}-{i}"\n'.encode())
                    assert stream.readline() == b"OK\n"

        with ThreadPoolExecutor(max_workers=2) as workers:
            tasks = [workers.submit(writer, prefix) for prefix in ("a", "b")]
            for task in tasks:
                task.result(timeout=10)
        assert primary_log.read_bytes() == replica_log.read_bytes()
        assert len(primary_log.read_text().splitlines()) == 103
        expected = request(primary_port, b'GET "shared"')
        assert request(replica_port, b'GET "shared"') == expected

        # A second peer cannot enter this one-session experiment.
        assert request(replica_port, b"REPLICA").startswith(b"ERROR SESSION_REJECTED")
    finally:
        if primary is not None:
            stop_server(primary)
        stop_server(replica)

    # Existing replica history is readable after restart, but not silently resynced.
    replica = start_server(executable, replica_log, replica_port, 30, "replica")
    try:
        assert request(replica_port, b'GET "shared"') == expected
        assert request(replica_port, b"REPLICA").startswith(b"ERROR SESSION_REJECTED")
    finally:
        stop_server(replica)

    restarted_primary = subprocess.run(
        [str(executable), str(primary_log), str(primary_port), "30", "primary", str(replica_port)],
        capture_output=True, timeout=3,
    )
    assert restarted_primary.returncode != 0
    assert b"fresh primary log" in restarted_primary.stderr


def test_peer_failure(executable, directory, behavior):
    """A fake replica controls ACK fragmentation, mismatch, loss, and shutdown."""
    primary_port = choose_port()
    received = threading.Event()
    release = threading.Event()
    peer_errors = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        replica_port = listener.getsockname()[1]
        while primary_port == replica_port:
            primary_port = choose_port()

        def peer():
            try:
                connection, _ = listener.accept()
                connection.settimeout(7)
                with connection, connection.makefile("rb") as stream:
                    assert stream.readline() == b"REPLICA\n"
                    connection.sendall(b"READY\n")
                    assert stream.readline() == b'SET "key" "local"\n'
                    received.set()
                    if behavior == "fragmented":
                        connection.sendall(b"O")
                        connection.sendall(b"K\n")
                        assert stream.readline() == b""
                    elif behavior == "wrong":
                        connection.sendall(b"NOT_FOUND\n")
                    elif behavior in ("shutdown", "timeout"):
                        assert release.wait(10)
                    # "lost" closes without sending an ACK.
            except Exception as error:
                peer_errors.append(error)

        thread = threading.Thread(target=peer)
        thread.start()
        primary = None
        try:
            primary = start_server(executable, directory / f"{behavior}.log",
                                   primary_port, 30, "primary", replica_port)
            with open_client(primary_port) as client, client.makefile("rb") as stream:
                client.settimeout(7)
                client.sendall(b'SET "key" "local"\n')
                assert received.wait(3)
                if behavior == "shutdown":
                    stop_server(primary)
                else:
                    response = stream.readline()
                    if behavior == "fragmented":
                        assert response == b"OK\n"
                    else:
                        assert response.startswith(b"ERROR REPLICATION_UNCERTAIN")
                        client.sendall(b'SET "key" "must not apply"\n')
                        assert stream.readline().startswith(b"ERROR REPLICA_UNAVAILABLE")
                        client.sendall(b'GET "key"\n')
                        assert stream.readline() == b'VALUE "local"\n'
        finally:
            release.set()
            if primary is not None:
                stop_server(primary)
            thread.join(timeout=8)
            assert not thread.is_alive()
        if peer_errors:
            raise peer_errors[0]


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="kv_replication_test_") as temporary:
        directory = Path(temporary)
        test_pair(executable, directory)
        for behavior in ("fragmented", "wrong", "lost", "timeout", "shutdown"):
            test_peer_failure(executable, directory, behavior)
    print("All first-session replication tests passed.")


if __name__ == "__main__":
    main()
