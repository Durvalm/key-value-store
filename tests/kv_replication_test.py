"""Numbered live replication tests; reconnect/catch-up is not implemented."""

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


def test_header_only_restart(executable, directory):
    primary_port, replica_port = distinct_ports()
    primary_log = directory / "header-primary.log"
    replica_log = directory / "header-replica.log"
    # Create headers, stop without mutations, then reopen using only the saved IDs.
    for history_id in ("header-session", None):
        replica = start_server(executable, replica_log, replica_port, 30, "replica",
                               history_id=history_id)
        primary = None
        try:
            primary = start_server(executable, primary_log, primary_port, 30, "primary",
                                   replica_port, history_id=history_id)
            assert primary_log.read_bytes() == replica_log.read_bytes() == b"HISTORY header-session\n"
            if history_id is None:
                assert request(primary_port, b'SET "color" "blue"') == b"OK\n"
                assert request(replica_port, b'GET "color"') == b'VALUE "blue"\n'
                assert primary_log.read_bytes() == replica_log.read_bytes()
        finally:
            if primary is not None:
                stop_server(primary)
            stop_server(replica)

    missing_id = subprocess.run(
        [str(executable), str(directory / "needs-id.log"), str(primary_port), "30", "replica"],
        capture_output=True, timeout=3,
    )
    assert missing_id.returncode != 0
    assert b"explicit history ID" in missing_id.stderr


def test_handshake_validation(executable, directory):
    primary_port, replica_port = distinct_ports()
    replica_log = directory / "handshake-replica.log"
    replica = start_server(executable, replica_log, replica_port, 30, "replica",
                           history_id="handshake-history")
    primary = None
    try:
        for command, error in (
            (b"REPLICA", b"ERROR INVALID_HANDSHAKE"),
            (b"REPLICA handshake-history extra", b"ERROR INVALID_HANDSHAKE"),
            (b"REPLICA handshake-history\r", b"ERROR INVALID_HANDSHAKE"),
            (b"REPLICA unrelated-history", b"ERROR HISTORY_MISMATCH"),
        ):
            with open_client(replica_port) as client, client.makefile("rb") as stream:
                client.sendall(command + b"\n")
                assert stream.readline().startswith(error)
                assert stream.readline() == b""
            assert replica_log.read_bytes() == b"HISTORY handshake-history\n"

        # The real primary must fail startup if the replica has a different ID.
        mismatched_log = directory / "mismatched-primary.log"
        mismatched = subprocess.run(
            [str(executable), str(mismatched_log), str(primary_port), "30", "primary",
             str(replica_port), "--history-id", "unrelated-history"],
            capture_output=True, timeout=3,
        )
        assert mismatched.returncode != 0
        assert b"HISTORY_MISMATCH" in mismatched.stderr
        assert mismatched_log.read_bytes() == b"HISTORY unrelated-history\n"

        # Rejected attempts must not claim the peer slot or bypass read-only rules.
        assert request(replica_port, b'SET "direct" "no"').startswith(b"ERROR READ_ONLY")
        primary_log = directory / "handshake-primary.log"
        primary = start_server(executable, primary_log, primary_port, 30, "primary",
                               replica_port, history_id="handshake-history")
        assert request(primary_port, b'SET "color" "blue"') == b"OK\n"
        assert request(replica_port, b'GET "color"') == b'VALUE "blue"\n'
        assert primary_log.read_bytes() == replica_log.read_bytes()
    finally:
        if primary is not None:
            stop_server(primary)
        stop_server(replica)


def test_bad_handshake_reply(executable, directory):
    for index, reply in enumerate((
        b"READY wrong-history 0\n", b"READY test-history 1\n",
        b"READY test-history\n", b"READY\n",
    )):
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(3)
            replica_port = listener.getsockname()[1]
            primary_port = choose_port()
            while primary_port == replica_port:
                primary_port = choose_port()

            def peer():
                try:
                    connection, _ = listener.accept()
                    connection.settimeout(3)
                    with connection, connection.makefile("rb") as stream:
                        assert stream.readline() == b"REPLICA test-history\n"
                        connection.sendall(reply)
                        assert stream.readline() == b""
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=peer)
            thread.start()
            log = directory / f"bad-handshake-{index}.log"
            try:
                result = subprocess.run(
                    [str(executable), str(log), str(primary_port), "30", "primary",
                     str(replica_port), "--history-id", "test-history"],
                    capture_output=True, timeout=5,
                )
                assert result.returncode != 0
                assert b"Unexpected replica acknowledgment" in result.stderr
                assert log.read_bytes() == b"HISTORY test-history\n"
            finally:
                thread.join(timeout=4)
                assert not thread.is_alive()
            if errors:
                raise errors[0]


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
        assert primary_log.read_bytes() == replica_log.read_bytes() == b"HISTORY test-history\n"

        assert request(primary_port, b'SET "name" "Durval"') == b"OK\n"
        assert request(replica_port, b'GET "name"') == b'VALUE "Durval"\n'
        assert request(primary_port, b'SET "name" "new value"') == b"OK\n"
        assert request(replica_port, b'GET "name"') == b'VALUE "new value"\n'
        assert request(primary_port, b'DELETE "name"') == b"INTEGER 1\n"
        assert request(replica_port, b'GET "name"') == b"NOT_FOUND\n"
        assert len(primary_log.read_text().splitlines()) == 4
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
        assert len(primary_log.read_text().splitlines()) == 104
        assert [int(line.split()[0]) for line in primary_log.read_text().splitlines()[1:]] == list(range(1, 104))
        expected = request(primary_port, b'GET "shared"')
        assert request(replica_port, b'GET "shared"') == expected

        # A second peer cannot enter this one-session experiment.
        assert request(replica_port, b"REPLICA test-history").startswith(b"ERROR SESSION_REJECTED")
    finally:
        if primary is not None:
            stop_server(primary)
        stop_server(replica)

    # Existing replica history is readable after restart, but not silently resynced.
    replica = start_server(executable, replica_log, replica_port, 30, "replica")
    try:
        assert request(replica_port, b'GET "shared"') == expected
        assert request(replica_port, b"REPLICA test-history").startswith(b"ERROR SESSION_REJECTED")
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
                    assert stream.readline() == b"REPLICA test-history\n"
                    connection.sendall(b"READY test-history 0\n")
                    assert stream.readline() == b'1 SET "key" "local"\n'
                    received.set()
                    if behavior == "fragmented":
                        connection.sendall(b"ACK ")
                        connection.sendall(b"1\n")
                        assert stream.readline() == b""
                    elif behavior == "wrong":
                        connection.sendall(b"ACK 2\n")
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


def test_numbered_peer(executable, directory):
    port = choose_port()
    log = directory / "numbered-peer.log"
    replica = start_server(executable, log, port, 30, "replica")
    try:
        with open_client(port) as client, client.makefile("rb") as stream:
            client.sendall(b"REPLICA test-history\n")
            assert stream.readline() == b"READY test-history 0\n"
            # Fragmented input, followed by multiple complete records in one send.
            client.sendall(b'1 SET "color" ')
            client.sendall(b'"blue"\n2 SET "color" "red"\n')
            assert stream.readline() == b"ACK 1\n"
            assert stream.readline() == b"ACK 2\n"
            before = log.read_bytes()
            client.sendall(b'1 SET "color" "blue"\n')
            assert stream.readline() == b"ACK 1\n"
            assert log.read_bytes() == before
            assert request(port, b'GET "color"') == b'VALUE "red"\n'
            client.sendall(b'3 DELETE "color"\n')
            assert stream.readline() == b"ACK 3\n"
            client.sendall(b'3 DELETE "color"\n')
            assert stream.readline() == b"ACK 3\n"
            # An old conflicting record closes the peer without changing its history.
            before = log.read_bytes()
            client.sendall(b'2 SET "color" "wrong"\n')
            assert stream.readline().startswith(b"ERROR REPLICATION_RECORD")
            assert stream.readline() == b""
            assert log.read_bytes() == before
    finally:
        stop_server(replica)

    for index, record in enumerate((
        b'2 SET "k" "gap"', b'0 SET "k" "zero"',
        b'-1 SET "k" "negative"', b'18446744073709551616 SET "k" "overflow"',
        b'1 GET "k"', b'1 SET k unquoted', b'1 DELETE "missing"',
        b'1 SET "k" "' + b'x' * 4090 + b'"',
    )):
        log = directory / f"bad-peer-{index}.log"
        replica = start_server(executable, log, port, 30, "replica")
        try:
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b"REPLICA test-history\n")
                assert stream.readline() == b"READY test-history 0\n"
                client.sendall(record + b"\n")
                assert stream.readline().startswith(b"ERROR REPLICATION_RECORD")
                assert log.read_bytes() == b"HISTORY test-history\n"
        finally:
            stop_server(replica)


def test_maximum_command(executable, directory):
    primary_port, replica_port = distinct_ports()
    replica = start_server(executable, directory / "max-replica.log", replica_port, 30, "replica")
    primary = None
    try:
        primary = start_server(executable, directory / "max-primary.log", primary_port,
                               30, "primary", replica_port)
        command = b'SET "k" "' + b'x' * (4096 - len(b'SET "k" ""')) + b'"'
        assert len(command) == 4096
        assert request(primary_port, command) == b"OK\n"
        assert request(replica_port, b'GET "k"') == b'VALUE "' + b'x' * 4086 + b'"\n'
    finally:
        if primary is not None:
            stop_server(primary)
        stop_server(replica)


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="kv_replication_test_") as temporary:
        directory = Path(temporary)
        test_header_only_restart(executable, directory)
        test_handshake_validation(executable, directory)
        test_bad_handshake_reply(executable, directory)
        test_pair(executable, directory)
        test_numbered_peer(executable, directory)
        test_maximum_command(executable, directory)
        for behavior in ("fragmented", "wrong", "lost", "timeout", "shutdown"):
            test_peer_failure(executable, directory, behavior)
    print("All numbered replication tests passed.")


if __name__ == "__main__":
    main()
