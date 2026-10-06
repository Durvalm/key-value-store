# Part 4: Concurrency Requirements

## Purpose

Allow multiple clients to use the same database while preserving correct results,
persistence, and predictable resource use.

This document defines behavior and acceptance tests. Class structure, function
signatures, lock placement, and thread ownership are design work for the learner.
Work through one checkpoint at a time; do not implement the whole document at once.

## Learning Objectives

- Distinguish a process from a thread and identify which memory threads share.
- Explain concurrency versus parallelism and why an idle socket need not block other clients.
- Distinguish a data race (unsynchronized conflicting memory accesses) from a logical race (an incorrect interleaving of otherwise valid steps).
- Explain mutual exclusion, critical sections, lock ownership, and automatic lock release.
- Explain why a const method is not automatically thread-safe.
- Recognize deadlock, contention, and the cost of holding a lock during slow work.
- Explain thread lifetime, joining, bounded queues, and backpressure.
- Explain why persistence ordering is part of concurrency correctness.

## Starting Point

The current server serves one connection until it closes before serving another.
It already has framing, partial-send handling, configurable endpoints, persistence,
an inactivity timeout, and signal handlers.

`KeyValueStore` currently has no synchronization. Its hash map, append-only log,
and compaction temporary file cannot safely be manipulated by independent handlers
without coordination. One server process must continue to own one shared store.
Replay completes before any worker can access that store.

The existing `SO_RCVTIMEO` policy limits each blocking receive wait. It does not
limit the total time to assemble a request if bytes keep arriving. The existing
signal flag also does not by itself provide worker-thread coordination or wake
every blocked operation. Revisit both assumptions when adding workers.

## Existing Behavior to Preserve

- Preserve Protocol V1 commands, quoted fields, responses, and request size limit.
- Preserve ordered requests and responses within each connection.
- Preserve the CLI and client interfaces; document any new server configuration.
- Preserve restart recovery and compaction behavior under the existing durability model.
- Keep the server loopback-only by default.
- Preserve failure reporting for invalid configuration, port conflicts, and inaccessible logs.
- Keep current storage, protocol, and network tests passing.

## Phase 4A: Shared State and Correctness

### C1. Concurrent Connections

With capacity for at least two active clients, client B must receive a response
while client A remains connected and idle. Each connection owns its own input
buffer, parser temporaries, and response bytes. Responses must never reach the
wrong client or interleave with another response on the same connection.

Process one request at a time per connection. Parallel requests within one
connection are outside this phase.

### C2. Atomic Store Operations

The public store operations must be safe when called from multiple threads on the
same instance, including direct calls outside the server command processor.

Each completed operation must behave as if it happened at one point between its
invocation and return. This property is called linearizability. In practical terms:

- A GET sees a complete value or a missing-key result, never a partially modified value.
- A read started after an acknowledged SET sees that value unless a later mutation changes it.
- Concurrent writes to one key may finish in either valid order; do not assume a winner based on thread creation order.
- If an existing key receives two concurrent DELETE calls with no intervening SET, exactly one returns success.
- SIZE and EXISTS observe a coherent store state while other threads mutate it.
- Returned values remain valid after the operation ends and another thread changes the key.

A sequence of separate commands is not a transaction. GET followed by SET does
not become an atomic increment simply because each operation is thread-safe.

A single mutex protecting store operations is an acceptable first solution.
Choose correctness before optimizing simultaneous reads or independent keys.

### C3. Persistence and Compaction

Protect the relationship between memory and disk, not only accesses to the map.
Log records must remain complete and replay in an order consistent with the
final in-memory state after successful concurrent operations.

For example, this schedule is incorrect:

```text
A appends SET key A
B appends SET key B
B updates memory to B
A updates memory to A
Restart replays B, but memory before restart held A.
```

COMPACT may temporarily block other store operations. It must not overwrite a
concurrent mutation or race another compaction using the same temporary path.
After successful compaction and shutdown, replay must reconstruct the same state.

Preserve existing handling of storage errors and release synchronization resources
when an exception occurs. Do not claim stronger power-loss durability or rollback
guarantees than the persistence layer currently provides.

### C4. Lock Scope and Ownership

Document the shared resources and the synchronization rule for each one. Explain
who owns every accepted socket, who closes it, and how ownership transfers.

Do not hold the store lock while waiting for terminal input, receiving socket
bytes, or sending responses. An idle client or slow reader must not monopolize
access to the database. Disk I/O inside the store's critical section is acceptable
for this first version if its performance tradeoff is explained.

Avoid accidental recursive locking when a public operation calls a helper. Locks
must be released on success, early return, and exceptions.

## Phase 4B: Bounded Workers

### C5. Worker Pool and Admission

The final implementation must use a fixed, configurable number of worker threads
and a bounded queue of accepted connections. A limited thread-per-connection
experiment is allowed before this checkpoint.

- Document worker count and queue capacity defaults, allowed ranges, and configuration.
- Reject invalid configuration with a failure status.
- Bound the number of active and queued accepted connections, as well as retained thread objects.
- Document behavior when capacity is exhausted. Promptly closing an excess connection is acceptable; a bounded error response is optional.
- Do not confuse the application's connection queue with the kernel's listen backlog.
- Workers waiting for work must sleep instead of continuously checking an empty queue.
- Return worker capacity after disconnects, errors, timeouts, and EXIT.

Backpressure means limiting or rejecting new work when the server cannot keep up.
It does not require a new distributed message queue or broker.

### C6. Slow and Failing Clients

Specify a finite deadline for receiving a complete request, including clients that
send bytes slowly without a newline. Also bound waits when sending responses to
clients that stop reading. A queued connection must not remain retained forever.

These policies must release capacity without affecting unrelated clients or
executing incomplete commands. Request size and error policies must be consistent
regardless of where TCP divides the incoming bytes.

Contain recoverable connection failures within that session. A disconnected peer
must not terminate the server through SIGPIPE. Account for exceptions in worker
functions; an exception handler in main does not catch another thread's exceptions.

## Phase 4C: Shutdown and Verification

### C7. Coordinated Shutdown

On SIGINT or SIGTERM, stop admitting new work, release queued connections, and
ensure idle workers and workers blocked on socket I/O can stop. Do not assume a
signal delivered to one thread interrupts every thread's system calls.

Document whether an operation already executing is allowed to finish. Never
execute incomplete input or send a success response for an operation that did
not complete. A mutation can have completed even if shutdown prevents its
response from reaching the client; automatic retries are outside this phase.

Join workers before destroying the shared store or their synchronization objects.
Do not detach threads that may outlive those resources. Socket descriptors must
have a clear closing owner, including error paths; avoid double-closing a descriptor
that the OS may already have reused.

Shutdown must finish within a documented bound in tests with idle, queued, and
network-stalled clients. This does not promise a deadline for arbitrarily hung
filesystem operations. Signal handlers must remain limited to signal-safe work;
`volatile` is not general synchronization between threads.

## Required Tests

Use isolated temporary logs, selected test ports, and finite test deadlines.
Coordinate concurrent starts with barriers/events where useful. Arbitrary sleeps
alone do not prove that two operations overlapped.

- Keep A connected and idle while B completes SET and GET, before A's timeout.
- Interleave fragmented requests from two connections without mixing their buffers or responses.
- Write distinct keys from several clients and verify every expected value and the exact final size.
- Race two DELETE operations on an existing key and verify exactly one succeeds.
- Race same-key writes and reads; verify valid whole values without assuming which overlapping writer wins.
- After all concurrent mutations finish, record the live state, restart, and compare recovered state.
- Run COMPACT alongside mutations and another COMPACT; compare recovered state after all operations finish.
- Inject a recoverable store failure and verify locks are released and subsequent operations can proceed.
- Exceed configured active and queued capacity; verify the documented overload policy and later capacity recovery.
- Exercise idle clients, slowly arriving incomplete requests, and clients that do not read responses.
- Signal shutdown with active and queued clients; verify timely exit, joined workers, and recovery of acknowledged writes.
- Exercise the actual kv_client against the concurrent server and preserve existing tests.

Use direct C++ threads for store tests as well as network clients for server tests.
Run ThreadSanitizer in a separate supported build and investigate race reports.
Record platform/tool limitations if it cannot run. Passing tests or a clean
sanitizer run supports correctness but is not a proof that every interleaving is safe.

## Implementation Checkpoints

1. **Observe the limitation:** demonstrate that B waits while A stays connected in the current server. Predict what must change.
2. **Learn thread lifetime:** make a small isolated two-thread experiment. Explain shared memory, captured references, and joining before changing the server.
3. **Protect the store:** define the atomic operation boundary and pass direct concurrent storage and replay tests.
4. **Serve two clients:** implement a bounded concurrent connection experiment with clear ownership and session cleanup.
5. **Bound and reuse workers:** add the worker pool, waiting queue, overload behavior, and receive/send deadlines.
6. **Shut down and verify:** coordinate worker exit, run integration and race-detection tests, and document guarantees.

## Learning Rules

Before each checkpoint, write the behavior you want, the shared state involved,
and two possible failure schedules. Ask for a small explanation or experiment
when a new API is unfamiliar.

When introducing a C++, POSIX, or kernel mechanism, explain its purpose, inputs,
outputs, ownership, blocking behavior, and failure results before presenting the
implementation. Separate API facts that must be looked up from design decisions
the learner can reason through. Do not introduce a complete threading framework
before the basic lifecycle is understood.

Useful questions:

- Can another thread read or change this object right now?
- Which steps must happen together for replay and memory to agree?
- Am I holding a database lock while waiting on a client?
- Who wakes this worker, and what happens if shutdown starts while it sleeps?
- What limits memory, sockets, and threads when clients arrive faster than service?

## Non-Goals

- Lock-free containers, custom atomic algorithms, or per-key locking.
- Guaranteed throughput improvements or a benchmark target; measurement is Part 5.
- Asynchronous networking frameworks, parallel commands within a connection, or changing the wire protocol.
- Cross-command transactions, MVCC, or snapshot isolation.
- Multiple server processes sharing one persistence file.
- Replication, sharding, consensus, or distributed message brokers.
- New crash/power-loss durability guarantees.

## Definition of Done

Multiple clients make progress within configured capacity. Store operations are
safe under concurrency, memory and recovery agree, and compaction preserves writes.
Resources and network waits are bounded. Shutdown releases connections and joins
workers before destroying shared state. Existing and new tests pass, race-detection
results are recorded, and the learner can explain one command from acceptance
through synchronization, persistence, response, and cleanup.
