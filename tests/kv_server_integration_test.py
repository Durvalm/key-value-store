import socket
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


SERVER_HOST = "127.0.0.1"


def choose_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((SERVER_HOST, 0))
        return probe.getsockname()[1]


def start_server(executable, database_path, port, idle_timeout=1):
    process = subprocess.Popen(
        [str(executable), str(database_path), str(port), str(idle_timeout)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"server exited during startup with {process.returncode}")

        try:
            connection = socket.create_connection((SERVER_HOST, port), timeout=0.1)
            connection.close()
            return process
        except OSError:
            time.sleep(0.02)

    process.terminate()
    process.wait(timeout=2)
    raise AssertionError("server did not begin listening")


def stop_server(process):
    if process.poll() is not None:
        return

    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)

    if process.returncode != 0:
        raise AssertionError(f"server did not shut down cleanly: {process.returncode}")


def open_client(port):
    client = socket.create_connection((SERVER_HOST, port), timeout=1)
    client.settimeout(1)
    return client


def expect_line(stream, expected):
    actual = stream.readline()
    if actual != expected:
        raise AssertionError(f"expected {expected!r}, received {actual!r}")


def test_server(server_executable, client_executable):
    with tempfile.TemporaryDirectory(prefix="kv_server_test_") as temporary_directory:
        directory = Path(temporary_directory)
        database_path = directory / "server.log"
        port = choose_port()
        server = start_server(server_executable, database_path, port)

        try:
            # A request may be fragmented across multiple TCP writes.
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b'SET "fragmented" "works')
                time.sleep(0.02)
                client.sendall(b'"\n')
                expect_line(stream, b"OK\n")

            # Multiple complete requests may arrive in one TCP write.
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(
                    b'GET "fragmented"\n'
                    b'EXISTS "fragmented"\n'
                    b'SIZE\n'
                )
                expect_line(stream, b'VALUE "works"\n')
                expect_line(stream, b"INTEGER 1\n")
                expect_line(stream, b"INTEGER 1\n")

            # An incomplete request is discarded when its client disconnects.
            with open_client(port) as client:
                client.sendall(b'SET "incomplete" "must not execute"')

            time.sleep(0.02)
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b'GET "incomplete"\n')
                expect_line(stream, b"NOT_FOUND\n")

            # A complete oversized request reaches the protocol size check.
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall((b"X" * 4097) + b"\n")
                response = stream.readline()
                if not response.startswith(b"ERROR REQUEST_TOO_LARGE"):
                    raise AssertionError(f"unexpected oversized response: {response!r}")

            # An oversized request without a newline is bounded and disconnected.
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b"X" * 4097)
                response = stream.readline()
                if not response.startswith(b"ERROR REQUEST_TOO_LARGE"):
                    raise AssertionError(f"unexpected unterminated oversized response: {response!r}")
                if stream.read(1) != b"":
                    raise AssertionError("oversized unterminated request did not close the connection")

            # An idle client is released without stopping the server.
            with open_client(port) as client:
                client.settimeout(3)
                if client.recv(1) != b"":
                    raise AssertionError("idle connection returned unexpected data")

            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b"SIZE\n")
                expect_line(stream, b"INTEGER 1\n")

            # The project client can execute multiple requests and exit cleanly.
            client_result = subprocess.run(
                [str(client_executable), SERVER_HOST, str(port)],
                input='SET "from-client" "works"\nGET "from-client"\nEXIT\n',
                text=True,
                capture_output=True,
                timeout=3,
                check=False,
            )
            if client_result.returncode != 0:
                raise AssertionError(
                    f"kv_client failed with {client_result.returncode}: {client_result.stderr}"
                )
            for expected in ("OK\n", 'VALUE "works"\n', "BYE\n"):
                if expected not in client_result.stdout:
                    raise AssertionError(
                        f"kv_client output missing {expected!r}: {client_result.stdout!r}"
                    )

            # EXIT closes this client but leaves the listening server alive.
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b"EXIT\n")
                expect_line(stream, b"BYE\n")
                if stream.read(1) != b"":
                    raise AssertionError("EXIT did not close the client connection")

            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b'SET "persistent" "yes"\n')
                expect_line(stream, b"OK\n")

            # A second server cannot bind the active server's port.
            conflict = subprocess.run(
                [str(server_executable), str(directory / "conflict.log"), str(port)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=3,
                check=False,
            )
            if conflict.returncode == 0:
                raise AssertionError("server reported success when bind failed")
        finally:
            stop_server(server)

        # Mutations made through TCP survive a normal server restart.
        server = start_server(server_executable, database_path, port)
        try:
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b'GET "persistent"\n')
                expect_line(stream, b'VALUE "yes"\n')
        finally:
            stop_server(server)

        # SIGINT interrupts a blocked recv without executing buffered input.
        server = start_server(server_executable, database_path, port)
        with open_client(port) as client:
            client.sendall(b'SET "shutdown-incomplete" "must not execute"')
            server.send_signal(signal.SIGINT)
            server.wait(timeout=2)
            if server.returncode != 0:
                raise AssertionError(f"server did not shut down cleanly: {server.returncode}")
            client.settimeout(2)
            try:
                remaining = client.recv(1)
            except ConnectionResetError:
                remaining = b""
            if remaining != b"":
                raise AssertionError("graceful shutdown did not close the active client")

        server = start_server(server_executable, database_path, port)
        try:
            with open_client(port) as client, client.makefile("rb") as stream:
                client.sendall(b'GET "shutdown-incomplete"\n')
                expect_line(stream, b"NOT_FOUND\n")
        finally:
            stop_server(server)

        invalid_port = subprocess.run(
            [str(server_executable), str(database_path), "70000"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=3,
            check=False,
        )
        if invalid_port.returncode == 0:
            raise AssertionError("server accepted an out-of-range port")

        inaccessible_log = subprocess.run(
            [
                str(server_executable),
                str(directory / "missing-directory" / "server.log"),
                str(choose_port()),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=3,
            check=False,
        )
        if inaccessible_log.returncode == 0:
            raise AssertionError("server reported success with an inaccessible persistence path")

        invalid_timeout = subprocess.run(
            [str(server_executable), str(database_path), str(choose_port()), "0"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=3,
            check=False,
        )
        if invalid_timeout.returncode == 0:
            raise AssertionError("server accepted an invalid idle timeout")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: kv_server_integration_test.py <kv_server> <kv_client>")

    test_server(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
    print("All KV server integration tests passed.")


if __name__ == "__main__":
    main()
