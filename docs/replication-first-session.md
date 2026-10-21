# Live replication and startup recovery

A primary forwards mutations over one persistent connection and checks the
replica's numbered acknowledgment before returning success to the client. Both
nodes replay their own numbered logs on restart. Startup then verifies the
replica's retained records and sends its missing mutations before the primary
starts its client workers. Existing logs can now be reused.

Automatic reconnect, history pruning, and failover remain outside this checkpoint;
see the [replication requirements](replication-requirements.md).

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

The primary prints the replica's reported sequence, verification/catch-up counts,
and `Replica ready at sequence N` before printing
`Connected to replica on 127.0.0.1:6381`. Starting the replica does not connect it to itself.

Choose a new ID for each independent experiment and supply that same ID to both
nodes. IDs contain 1–128 letters, digits, dots, underscores or hyphens. New numbered
logs require `--history-id`; reopening an existing header restores its ID without
the option. If supplied on reopen, the ID must match the saved one. The option
must come last and is only for primary/replica roles. There is no automatic ID
generation or automatic ID adoption. The replica rejects a different history ID,
and the primary validates the reply's ID and actual sequence. A replica ahead of
the primary is rejected rather than reset or overwritten.

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
   configures five-second per-call I/O timeouts, sends `REPLICA <history-id>\n`,
   and parses `READY <history-id> <last-sequence>\n`. The ID must match; the
   replica's progress may be behind or equal, but cannot be ahead. The primary
   supplies its ID and a copy of its retained records, not the store object.
   `exchange_message()` returns one response line; `send_to_replica()` separately
   checks exact ACKs. On failure the connection function closes its socket. It
   returns the owned descriptor to `run_server()` only after verification/catch-up.
2. **Peer versus ordinary client:** the replica parses the ID, rejects missing or
   extra arguments and different IDs, and permits only one peer session per process. Rejected
   handshakes do not consume that session. Its READY reply uses its own saved ID
   and progress. Only an accepted session can apply mutations. Ordinary replica client
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
   `exchange_message()` to send fully and receive a line, then checks the ACK. The
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

## Startup verification and catch-up

The fresh-mutation-history restrictions have been removed. Both processes first
replay their saved IDs, data, and sequence progress. The replica then accepts one
peer session even when it already has mutations.

For a primary with records 1–5 and a replica through 3:

```text
Primary sends REPLICA experiment-a
Replica replies READY experiment-a 3
Primary rejects wrong IDs or replica-ahead progress
Primary sends 1, 2, 3 → replica compares saved records and ACKs without reapplying
Primary sends 4, 5    → replica appends, flushes, applies, then ACKs
Primary prints Replica ready at sequence 5
Primary starts client workers and accepts new commands
```

The primary copies `history_records()` once under the store mutex. No store lock
is held across network I/O. It sends the entire copy in order: this deliberately
reuses duplicate verification instead of introducing another comparison protocol.
Only records missing from the replica are appended or applied. A conflict in the
retained prefix stops startup before any missing suffix is transferred. A replica
with no records receives the complete history. Matching IDs and sequence numbers
alone are not treated as proof of matching records.

The primary's listening socket is established before recovery, so a TCP client
may connect, but its commands are not processed until recovery finishes. Replica
reads remain available and may observe its older state or intermediate catch-up
progress. Console output reports startup progress; it is not a lag dashboard.

## Restart using the same files

After the first run, stop both servers with Ctrl-C. In their original terminals,
restart with the saved paths, without running `mktemp` again:

```sh
# Terminal 1
./build-release/kv_server "$replica_log" 6381 120 replica

# Terminal 2
./build-release/kv_server "$primary_log" 6380 120 primary 6381
```

The ID is restored from each header. Verification of identical histories does
not append duplicates, and subsequent writes continue sequence numbering.
To attach a fresh replica to an existing primary, create a new replica file and
supply the primary's saved ID with `--history-id`; automatic ID adoption is not
implemented.

If recovery disconnects after the replica persisted a record but before its ACK
arrives, primary startup fails. Restart the replica process and retry the primary
with the same files: replay restores the replica's actual position, matching
resends are verified, and the remaining suffix is transferred. No rollback or
blind overwrite occurs. A locally persisted write whose earlier client response
was uncertain can become replicated during this recovery; uncertainty never
promised that the write was absent.

We still allow only one accepted peer session per replica process. After a peer
has disconnected, restart the replica before reconnecting the primary. Rejected
malformed/wrong-ID attempts do not consume that session. There is no automatic
retry or heartbeat: a live idle connection can expire under the configured receive
timeout. A failure during normal replicated writes disables further primary
mutations until the processes are restarted and recovery succeeds.

COMPACT remains disabled for numbered history. Standalone behavior is unchanged.
Stream flushes are checked, but this does not add fsync or a power-loss guarantee.
Per-call timeouts are not absolute recovery deadlines; this implementation targets
small retained histories. History compaction, indexing and efficient transfers
remain deferred. Never run two processes against the same log file.

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
small experiment. Startup verifies the saved prefix before extending it. Both
nodes retain the full history in memory; the primary also takes a temporary copy
for startup recovery.

## Checks and the next learning step

Run:

```sh
ctest --test-dir build-release --output-on-failure
```

The replication tests check live SET/overwrite/DELETE, ordinary replica
read-only behavior, non-mutation filtering, identical log order with two writers,
restart recovery, fragmented/wrong/lost/timed-out ACKs,
and shutdown during a peer wait. They also check numbered ACKs, duplicate/conflicting
resends, gaps, fragmented/batched records, and a full-size client command with its
added sequence prefix. C++ tests check recovered progress, continued numbering,
concurrent numbering, incompatible logs, and incomplete numbered tails. Existing
standalone storage/network suites remain in place.

Header tests cover missing/existing-empty files, preserved IDs across restart,
header-only progress, supplied-ID mismatches, malformed headers, and reopening
header-only logs through the server without resupplying an ID.

Handshake tests cover matching and different IDs, malformed requests, rejected
attempts leaving the peer slot available, and primary rejection of wrong IDs,
replica-ahead progress, malformed numbers, or incomplete READY replies.

Recovery tests cover an empty replica, partial and equal histories, restart followed
by continued writes, conflicting histories even when current values agree, and a
replica ahead of the primary. A controlled fake peer persists a catch-up record
and drops its ACK; client writes remain blocked during that startup attempt, and
a retry using the persisted prefix converges without duplicate records.

The persistence-failure test starts only the replica under `RLIMIT_FSIZE`, with
room for the header and first SET. A small Python launcher sets the child process's
file-size limit, ignores `SIGXFSZ` so the write returns an error instead of killing
the process, then uses `exec` to run the normal server. The primary and test runner
are unaffected. No production failure switch is added.

The next SET persists locally on the primary but cannot append on the replica.
The test checks the uncertain client response, unchanged replica value/log,
locally updated primary value, rejection of later SET/DELETE, and clean shutdown.
Restarting without the test limit must catch up the saved prefix and resume
numbering without duplicates. This exercises a real failed append/flush with no
remaining file space; it does not simulate every partial-write or hardware failure.

Remaining Phase 6 work includes broader failure validation and a short results
report with recovery transcripts and a standalone/replicated latency comparison.
Automatic reconnect and failover remain separate extensions.
