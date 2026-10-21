# First live replication session

This is the next small implementation step: a primary forwards successful
mutations over one persistent connection and checks the receiver's reply before
returning success to the client. Mutations now have persistent sequence numbers
and numbered acknowledgments. It is still a fresh-session experiment, not the full
[replication requirements](replication-requirements.md): history identity is now
stored locally, but identity/progress exchange, reconnection, and catch-up are
not implemented yet.

## Run it

Stop previous servers using these ports, then build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

In terminal 1, start the replica with a fresh log:

```sh
replica_log=$(mktemp)
./build-release/kv_server "$replica_log" 6381 120 replica --history-id experiment-a
```

In terminal 2, start the primary with a separate fresh log:

```sh
primary_log=$(mktemp)
./build-release/kv_server "$primary_log" 6380 120 primary 6381 --history-id experiment-a
```

The primary should print `Connected to replica on 127.0.0.1:6381` after completing
the peer handshake. Starting the replica does not connect it to itself.

Choose a new ID for each independent experiment and supply that same ID to both
nodes. IDs contain 1–128 letters, digits, dots, underscores or hyphens. New numbered
logs require `--history-id`; reopening an existing header restores its ID without
the option. If supplied on reopen, the ID must match the saved one. The option
must come last and is only for primary/replica roles. There is no automatic ID
generation or network identity validation in this storage checkpoint; matching
IDs are currently the operator's responsibility.

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
kv_server [log] [port] [idle-timeout-seconds] [standalone|primary|replica] [replica-port] [--history-id id]
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
4. **Send and acknowledge:** the store assigns consecutive numbers while holding
   its mutex and persists each number with its mutation. The primary sends, for
   example, `7 SET "color" "blue"\n` and expects `ACK 7\n`. Both logs use the same
   numbered format. `send_to_replica()` adds the framing newline and uses
   `exchange_message()` to send fully, receive a line, and check the ACK. The
   replica's `apply_replica_record()` validates order, appends/flushes, and applies
   the mutation before the handler sends its ACK. The primary then sends the
   ordinary `OK\n` or `INTEGER 1\n` to its application client.
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

We have sequence numbers and a saved history ID, but not identity/progress exchange
or a catch-up protocol. Silently reconnecting
to arbitrary existing data could leave the maps different while new SETs appear
to succeed. This stage therefore refuses a primary with existing log contents
and refuses a replica peer session if the replica has existing mutations or has
already accepted a session in that process. Even a log ending with zero live keys
is existing mutation history, not a fresh log. A header-only log has sequence 0
and can reopen as a fresh session; the header itself is not a mutation.

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

The restarted replica replays the ID, map and sequence position, but rejects a
new peer session until catch-up is implemented. To inspect the primary log after
restart, stop both servers and open the primary's log in `replica` mode on a free
port for read-only access. `standalone` uses the old unnumbered format and cannot
open a numbered log. Do not create a new temporary path for recovery checks.
Stop servers before deleting their test logs.

## Numbered records: what is implemented

Client syntax is unchanged: `SET "color" "blue"`, without a number. Only actual
mutations consume a sequence number; reads and missing DELETEs do not. There is
no `REPL` prefix: the established peer session determines how to parse the line.

`KeyValueStore(file_path, true, "experiment-a")` creates numbered history with an
explicit ID for primary and replica. `KeyValueStore(file_path, true)` reopens an
existing header and recovers its ID. For example, the file contains:

```text
HISTORY experiment-a
1 SET "color" "blue"
2 SET "color" "red"
```

`history_id()` exposes the saved identity. The header is written/flushed once on
creation and validated during replay. Missing, malformed, incomplete and repeated
headers are rejected; the header does not enter the `history_` mutation vector.

`append_record()` writes the number and command as one record. Local replay
requires consecutive numbers beginning at 1 and reconstructs `last_sequence()`.
It does not append those records again. The retained records also let the replica
verify an old resend: identical mutation means ACK without reapplying or logging;
different contents mean rejection. Gaps, malformed records, and replicated DELETEs
of absent keys are rejected. Peer protocol errors close that connection.

Use fresh replication logs after this format change. Older numbered logs without
a header are rejected without migration. Legacy unnumbered logs remain usable in
standalone mode. A numbered log with an
incomplete final record is refused rather than silently appending after broken
bytes. Persistence/application failures disable further store mutations.

We retain the whole numbered history in memory for simple duplicate checks. No
log index, snapshot, pruning, or generic protocol framework is needed for this
small experiment. The saved history identity must still be exchanged and validated
before supporting connections to existing histories; a matching sequence alone
is insufficient.

## Checks and the next learning step

Run:

```sh
ctest --test-dir build-release --output-on-failure
```

The replication tests check live SET/overwrite/DELETE, ordinary replica
read-only behavior, non-mutation filtering, identical log order with two writers,
restart reads, existing-history rejection, fragmented/wrong/lost/timed-out ACKs,
and shutdown during a peer wait. They also check numbered ACKs, duplicate/conflicting
resends, gaps, fragmented/batched records, and a full-size client command with its
added sequence prefix. C++ tests check recovered progress, continued numbering,
concurrent numbering, incompatible logs, and incomplete numbered tails. Existing
standalone storage/network suites remain in place.

Header tests cover missing/existing-empty files, preserved IDs across restart,
header-only progress, supplied-ID mismatches, malformed headers, and reopening
header-only logs through the server without resupplying an ID.

Next, exchange and validate history identity/progress, then implement catch-up,
following Steps 3–4 of the requirements. This checkpoint recovers identity and
sequence progress locally; it does not resume replication between processes that
already have mutations.
