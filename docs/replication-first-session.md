# First live replication session

This is the next small implementation step: a primary forwards successful
mutations over one persistent connection and checks the receiver's reply before
returning success to the client. It is a fresh-session experiment, not the full
[replication requirements](replication-requirements.md). Numbered history,
reconnection, and catch-up are not implemented yet.

## Run it

Stop previous servers using these ports, then build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

In terminal 1, start the replica with a fresh log:

```sh
replica_log=$(mktemp)
./build-release/kv_server "$replica_log" 6381 120 replica
```

In terminal 2, start the primary with a separate fresh log:

```sh
primary_log=$(mktemp)
./build-release/kv_server "$primary_log" 6380 120 primary 6381
```

The primary should print `Connected to replica on 127.0.0.1:6381` after completing
the peer handshake. Starting the replica does not connect it to itself.

In terminal 3:

```sh
./build-release/kv_client 127.0.0.1 6380
```

Enter:

```text
SET "color" "blue"
GET "color"
EXIT
```

Expect `OK`, `VALUE "blue"`, and `BYE`. Then read the other node:

```sh
./build-release/kv_client 127.0.0.1 6381
```

```text
GET "color"
SET "color" "red"
EXIT
```

GET returns `VALUE "blue"`; the direct client SET returns `ERROR READ_ONLY`.
Successful primary DELETEs replicate too. Deleting an absent key returns zero
and does not forward a mutation. No database value is copied by sharing files.

The full startup syntax is:

```text
kv_server [log] [port] [idle-timeout-seconds] [standalone|primary|replica] [replica-port]
```

Role defaults to `standalone`, preserving existing commands/tests. Only primary
accepts a replica-port argument; its default destination is 6381. Use different
primary and replica ports and separate log files.

## Understand the code in this order

1. **Connection and handshake:** `connect_to_replica()` creates/connects a socket,
   configures five-second per-call I/O timeouts, sends `REPLICA\n`, and requires
   `READY\n`. On failure it closes its socket. On success `run_server()` owns the
   descriptor and passes it to the workers.
2. **Peer versus ordinary client:** a replica accepts that marker on one fresh
   connection. Only that session can apply mutations. Ordinary replica client
   connections can read but cannot write. The marker distinguishes traffic;
   it is not authentication. This experiment assumes trusted local processes.
3. **Mutation selection:** `CommandResult::mutation_applied` defaults to false.
   `process_command()` sets it after SET succeeds, or when DELETE removes a key.
   The primary forwards only those results. Reads, invalid commands, and missing
   DELETEs do not trigger replication.
4. **Send and acknowledge:** `send_to_replica()` adds the framing newline, handles
   partial sends, receives through a newline, and validates the expected reply.
   SET expects `OK\n`; successful DELETE expects `INTEGER 1\n`. Those replies
   follow receiver log append/flush checks and map application.
5. **Write order:** the primary's `replication_mutex` covers command execution and
   the complete peer exchange. Two workers cannot apply writes in one order and
   send them in another, or consume each other's replies. This simple choice also
   makes primary reads wait behind a pending replication exchange. Socket reads
   from clients happen outside this mutex.
6. **Failure:** after a locally applied write loses confirmation, return
   `ERROR REPLICATION_UNCERTAIN`. Record the session as failed; later primary
   SET/DELETE requests get `ERROR REPLICA_UNAVAILABLE` before changing memory.
   Reads remain available. Do not retry the failed write assuming it was absent.
7. **Shutdown:** main shuts down the outgoing socket to interrupt peer I/O, joins
   workers, then closes the socket. Workers borrow it and do not close it.

The receiver uses the existing store: append/flush/check, apply memory, then reply.
The request/reply logic is in `replica_connection.cpp`; connection lifetime and
worker coordination are in `server_main.cpp`. No new networking class is needed.

## Why fresh logs and one session?

We do not yet have sequence numbers or a catch-up protocol. Silently reconnecting
to arbitrary existing data could leave the maps different while new SETs appear
to succeed. This stage therefore refuses a primary with existing log contents
and refuses a replica peer session if the replica has existing history or has
already accepted a session in that process. Even a log ending with zero live keys
is existing history, not a fresh log.

A disconnected peer needs a new, deliberately prepared experiment; it is not
automatically reconnected. No heartbeat is implemented: an idle receiver can close
the peer connection after its configured receive timeout, and the primary may
discover that only on its next write. Readiness is not a promise that a network
connection cannot fail after local application.

COMPACT is disabled on primary and replica during this experiment. Standalone
behavior remains available. Keep local stream flushes; this does not add an fsync
or power-loss guarantee. Per-call timeouts are not absolute request deadlines.

To check normal restart recovery without attempting a new replication session:

1. Stop primary and replica with Ctrl-C in their respective terminals.
2. In terminal 1, restart the replica using **the same** `$replica_log` command.
3. Connect a client and GET `color`; it should still be `blue`.

The restarted replica can serve existing reads, but rejects a new peer session
until numbered recovery is implemented. To inspect the primary log after restart,
open that same log in `standalone` mode. Do not create a new temporary path for
these recovery checks. Stop servers before deleting their test logs.

## Checks and the next learning step

Run:

```sh
ctest --test-dir build-release --output-on-failure
```

The new replication tests check live SET/overwrite/DELETE, ordinary replica
read-only behavior, non-mutation filtering, identical log order with two writers,
restart reads, existing-history rejection, fragmented/wrong/lost/timed-out ACKs,
and shutdown during a peer wait. Existing standalone storage/network suites also
pass.

Next, replace the fresh-session restriction with numbered, persistent replication
history and ACKs. Only then add duplicate detection and reconnect/catch-up, following
Steps 3–4 of the requirements. This implementation establishes live forwarding;
it does not complete those recovery guarantees.
