# Replication results

Recorded 2026-10-21. This report covers the Phase 6 core: synchronous replication
between one primary and one replica, persistent numbered history, and startup
verification/catch-up. See the [walkthrough](replication-first-session.md) for the
code flow and [requirements](replication-requirements.md) for the intended scope.

## What works

- The primary orders successful SET and DELETE mutations, persists/applies them
  locally, forwards them, and checks the replica's numbered ACK before replying.
- Ordinary replica clients can read; direct writes are rejected.
- Both nodes replay their own logs to recover the history ID, data, and progress.
- Startup verifies the replica's existing records, applies missing mutations, and
  only then starts the primary's client workers.
- Wrong history IDs, conflicting records, sequence gaps, and a replica ahead of
  the primary fail visibly instead of silently merging or overwriting histories.

## Reproduction

Build and run the automated checks from the project directory:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Standalone, in its own terminal:

```sh
standalone_log=$(mktemp)
./build-release/kv_server "$standalone_log" 6380 3600 standalone
```

For replication, stop standalone first. Start the replica in terminal 1:

```sh
replica_log=$(mktemp)
./build-release/kv_server "$replica_log" 6381 3600 replica --history-id replication-bench-1
```

Start the primary in terminal 2:

```sh
primary_log=$(mktemp)
./build-release/kv_server "$primary_log" 6380 3600 primary 6381 --history-id replication-bench-1
```

The nodes use separate files and loopback TCP connections. Choose a different
history ID for each independent pair; both copies of one history share the ID.
Wait for `Replica ready at sequence ...` before benchmarking.

For either configuration, run in terminal 3:

```sh
./build-release/kv_benchmark 6380 SET 1
```

After a replicated run, read the final values directly from the replica:

```sh
./build-release/kv_benchmark 6381 VERIFY
```

For another fresh trial, stop the primary before the replica, rerun the `mktemp`
and startup commands, and use the next shared ID (`replication-bench-2`, then
`replication-bench-3`). For a recovery test, reuse the original file paths instead
of creating new ones.

## History and success semantics

A numbered log contains a header followed by consecutive mutations:

```text
HISTORY experiment-a
1 SET "color" "blue"
2 DELETE "color"
```

The handshake is `REPLICA <history-id>` followed by
`READY <history-id> <replica-last-sequence>`. Mutation messages use the same numbered
record format as the log; replies are `ACK <sequence>`. Each message ends with a
newline. Client commands remain unnumbered. Reads and missing-key DELETEs do not
create mutation records.

A normal successful replicated mutation means both nodes completed their log
append/flush checks and map application, and the primary received the expected
ACK. This is a stronger contract than standalone success, which confirms only
local processing. Stream flushing is not an fsync or power-loss guarantee.

`REPLICATION_UNCERTAIN` means the primary applied the write locally but could not
confirm replication. The replica may or may not have applied it. The primary
does not roll it back; reads can expose that value. Further mutations are denied
with `REPLICA_UNAVAILABLE`. Retrying an application request is not guaranteed to
execute exactly once.

## Recovery and failure examples

The following exchanges illustrate the behavior checked by the integration tests
and exercised manually. They are reproducible examples, not copied terminal logs.

Normal mutation and read path:

```text
Client → primary: SET "color" "blue"
Primary → replica: 1 SET "color" "blue"
Replica → primary: ACK 1
Primary → client: OK
Client → replica: GET "color"
Replica → client: VALUE "blue"
Client → primary: DELETE "color"
Primary → replica: 2 DELETE "color"
Replica → primary: ACK 2
Primary → client: INTEGER 1
Client → replica: GET "color"
Replica → client: NOT_FOUND
```

Startup with primary records 1–5 and replica records 1–3:

```text
Primary → replica: REPLICA experiment-a
Replica → primary: READY experiment-a 3
Replica reports sequence 3; verifying 3 records and catching up 2 records.
Records 1–3: compare with retained history, ACK without appending or applying again
Records 4–5: append, flush, apply, ACK
Replica ready at sequence 5
New client writes may now begin at sequence 6
```

The primary deliberately transmits the existing prefix for verification as well
as the missing suffix. Only missing records extend the replica's log. Restarting
an already matching pair verifies its history without adding duplicate records.
Restart the replica first, then the primary, using the same paths and saved IDs.

If the replica disconnects before a new write:

```text
SET "disconnected-test" "first" → ERROR REPLICATION_UNCERTAIN ...
GET "disconnected-test"         → VALUE "first"          (on primary)
SET "disconnected-test" "second" → ERROR REPLICA_UNAVAILABLE ...
```

Restarting the pair can transfer the locally persisted, previously uncertain
write. Likewise, if an ACK is lost after replica persistence, recovery verifies
that saved record rather than applying it twice.

Automated checks additionally cover two concurrent writers, malformed and
fragmented messages, conflicting histories with identical final values, a replica
ahead, interrupted catch-up, and blocked primary client writes during recovery.
The persistence-failure test limits only the replica's file size: the second
append fails, its map retains the old value, no successful ACK is returned, and
the primary reports uncertainty and denies later writes. Restarting without that
test limit must catch up and resume numbering. All three test suites passed after
that test was added.

## SET benchmark measurements

These are the six runs supplied by Durval. Fresh logs were used for every trial,
as confirmed by Durval. The runs used the Release build on his MacBook Pro, with
both nodes on the same machine over loopback. Detailed hardware and OS versions
were not recorded.

The workload was identical: one client, 100 total keys, 100 warm-up requests, and
100,000 measured SET requests. Preparation, warm-up, final verification, and
replica startup/catch-up are outside request timing. The benchmark waits for each
response before sending the next request. This measures one-client performance,
not maximum server capacity.

| Configuration | Run | Duration (s) | Throughput (requests/s) | Median (µs) | p95, nearest rank (µs) |
|---|---:|---:|---:|---:|---:|
| Standalone | 1 | 1.874 | 53,375.354 | 18.292 | 21.958 |
| Standalone | 2 | 1.894 | 52,807.521 | 18.416 | 22.500 |
| Standalone | 3 | 1.893 | 52,830.185 | 18.417 | 22.500 |
| Replicated | 1 | 3.989 | 25,071.299 | 39.292 | 44.791 |
| Replicated | 2 | 3.950 | 25,315.938 | 39.166 | 43.584 |
| Replicated | 3 | 4.012 | 24,927.161 | 39.333 | 45.875 |

| Average of the three reported run values | Standalone | Replicated |
|---|---:|---:|
| Throughput (requests/s) | 53,004.353 | 25,104.799 |
| Reported median latency (µs) | 18.375 | 39.264 |
| Reported p95 latency (µs) | 22.319 | 44.750 |

These latency averages summarize the reported run percentiles; they are not
percentiles calculated from a combined set of 300,000 samples.

The separate replica VERIFY output supplied after the first replicated run was:

```text
Verified final SET values for 100 keys (read-only).
```

The benchmark itself checks final SET values on the server being timed. The
separate VERIFY demonstrates those final values were also readable on the replica
for that run; it is not a complete comparison of both log histories.

## Findings and limits

- Replicated average throughput was about **52.6% lower** than standalone.
- The average reported median rose by **20.9 µs**, to approximately **2.14×** the
  standalone value. The reported p95 values were approximately doubled too.
- Within each configuration, the three runs were fairly consistent. More runs
  or different workloads would be needed for broader capacity conclusions.
- A replicated response waits for another process to receive, persist, apply,
  and acknowledge a mutation. With one outstanding request, the extra waiting
  also lowers throughput. The additional latency buys confirmation from both nodes.
- This comparison measures the whole implementation, including numbered logs,
  retained history, parsing, scheduling, network exchange, and replica persistence.
  It does not isolate how much time each component consumes or demonstrate that
  any one component is the bottleneck.
- These are local loopback results; they do not predict latency between machines
  or across regions, nor do they compare against a production database.

The implementation still retains complete history in memory and copies it for
startup recovery. Compaction is disabled for numbered logs. One accepted peer
session is allowed per replica process; restarting the replica is required before
opening another. There is no automatic reconnect, heartbeat, failover, quorum,
asynchronous acknowledgment policy, or power-loss durability guarantee. Timeouts
are per socket call, and testing does not cover every partial-write/hardware
failure. These are explicit limits of the learning milestone.
