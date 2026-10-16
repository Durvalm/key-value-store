# Part 6: Replication

The [first-session implementation](replication-first-session.md) now demonstrates
live mutation forwarding and acknowledgment. The numbered-history and recovery
requirements below remain later checkpoints, not completed features.

For the big picture and diagrams connecting this phase to later distribution work,
read the [Distributed Architecture Overview](distributed-architecture.md).

## What We Are Trying To Do

Run two separate copies of our database and make one follow the other's writes.
Start with one primary and one replica on this computer, each with its own process,
in-memory map, persistence file, and network endpoint.

The goal is to understand ordering, acknowledgment, recovery, and partial failure.
We are not trying to implement a production high-availability database or a
consensus algorithm. Build one checkpoint at a time. The first implementation
exercise is only sending and acknowledging one mutation.

This document defines the learning baseline and the behavior it must eventually
provide. It does not prescribe C++ classes, function signatures, or a thread
framework. Example message syntax is illustrative; choose and document a grammar
before implementing it.

## The First Mental Model: Replay On Another Process

Today, restarting the store reads a history such as:

```text
SET "language" "Python"
SET "language" "C++"
DELETE "temporary"
```

Applying those changes in order reconstructs the current state.

Replication uses the same idea across processes: as the primary accepts changes,
the replica receives and applies that history. The replica is not another thread
sharing our map, and the two processes must not share one writable log file.

```text
Client → primary → primary map and log
                 ↓ mutation messages over TCP
                replica → replica map and log
```

GET is not a mutation: we do not need to copy reads to reconstruct state. SET and
successful DELETE change state, so they belong in the replicated history.

**Make it click:** local replay says, “rebuild what happened here.” Replication
says, “apply what happened over there.” Both depend on knowing the order.

## Why Replicate, And What It Does Not Give Us Automatically

A separate copy can preserve access to data when one process is unavailable.
It can also serve reads, subject to how current its state is. On one computer,
both processes still share the machine's failure risks; this experiment teaches
the mechanism, not independence from power or disk failure.

A replica is also not a historical backup. If the primary accepts an unwanted
DELETE and we replicate correctly, the replica deletes it too.

Replication, durability, and availability are related but different:

| Concept | Question it answers |
| --- | --- |
| Local persistence | Can this process reconstruct its state after restart? |
| Replication | Can another process maintain a copy of the changes? |
| Acknowledgment policy | What must happen before the client is told success? |
| Availability policy | What can we still do when another node cannot be reached? |
| Backup | Can we recover an earlier state after an unwanted change? |

Our previous benchmark showed that changing persistence work changes latency.
Replication adds a network exchange and another store's persistence work, so the
meaning of `OK` becomes a performance decision as well as a correctness contract.

## Concepts To Learn Before Their Checkpoints

### Primary And Replica: One Place Chooses The Order

The primary accepts client writes. The replica accepts mutations from that primary
and ordinary read requests, but rejects direct client SET and DELETE commands.

Allowing both to accept unrelated writes would immediately raise conflict questions:
what if they both update the same key differently? We avoid that problem by giving
one process write authority. We will not automatically change that authority when
a connection fails.

### Sequence Numbers: Which Change Comes Next?

Give each accepted mutation a monotonically increasing number:

```text
1: SET "color" "red"
2: SET "color" "blue"
3: DELETE "color"
```

After applying records 1, 2, and 3, the key is absent. Reordering them can leave a
different state. The number identifies a position in history, not a timestamp.

If the replica has applied through 2, its next new record must be 3. A mutex gives
ordering within one process; sequence numbers communicate that order to another.
TCP orders bytes on one connection, but it does not remember application progress
across reconnections or order two independent client workers for us.

### An Acknowledgment Must Mean Something Specific

A successful `send()` only means the local OS accepted bytes. It does not tell us
that the replica received, parsed, persisted, or applied the mutation.

For this baseline, a replica acknowledgment for record n means:

> I have appended and flushed record n, applied it to my map, and have the complete
> ordered prefix through n under the normal-restart durability contract.

The primary can then decide whether it is ready to tell its client success.
We use “applied” for local state changes and “acknowledged” for a received reply;
we do not introduce a consensus commit index in this phase.

### Synchronous Versus Asynchronous: When Does The Client Stop Waiting?

| Policy | Before client success | What happens when the replica is unavailable? |
| --- | --- | --- |
| Our synchronous baseline | Primary append/flush/apply and replica append/flush/apply plus ACK | New mutations cannot report success; preflight-disconnected mutations are rejected |
| Possible later asynchronous mode | Primary append/flush/apply; replica can follow later | Writes may continue locally; successful writes may not yet exist on the replica |

Neither name alone specifies durability: we must still define whether an ACK
means received bytes, flushed history, or applied data. Our baseline chooses the
last two, using the same stream-flush/normal-restart guarantee as the current store.

These policies appear in real systems with more detailed contracts. PostgreSQL
supports synchronous standby acknowledgment choices; Redis documents its
asynchronous replication and the limits of waiting for replica acknowledgments.
Our two-process protocol does not acquire their additional machinery simply by
using a similar name. See [PostgreSQL standby replication](https://www.postgresql.org/docs/current/warm-standby.html)
and [Redis replication](https://redis.io/docs/latest/manual/replication/).

### Replication Lag: A Copy Can Be Behind

If the primary's last mutation is 12 and the replica's last applied mutation is 10,
the replica is behind by two records. This is sequence lag, not a measurement of
how many milliseconds it will take to catch up.

Even synchronous replication permits a brief interval while a new write is in
flight. The baseline promises that a successful write reply follows replica
application; it does not promise every concurrent read at both nodes always sees
identical state. A read issued on the replica after the client received success
must include that write or later ordered changes.

### Partial Failure: A Timeout Can Mean “I Don't Know”

Consider this sequence:

```text
Primary persists/applies record 7
Replica persists/applies record 7
Replica sends ACK 7
Connection fails before primary receives that ACK
```

The primary cannot report confirmed replicated success. But record 7 might exist
on both nodes. Removing it locally to “undo the timeout” would create a new problem.

**Make it click:** failing to hear “done” is different from knowing “not done.”
A distributed operation can have an uncertain outcome. We must expose that rather
than silently reporting either success or guaranteed absence.

## Assumptions That Keep This Phase Small

- One configured primary and one configured replica on loopback TCP.
- Separate temporary persistence paths and chosen ports; no personal database files.
- Start the replication deployment with empty files, not silently converted legacy logs.
- Existing standalone mode keeps its current protocol, compaction, and persistence behavior.
- Small human-readable records; explicit framing, quoting, and finite message limits.
- One outstanding primary-to-replica mutation at a time; no batches or pipeline.
- First networking checkpoint uses one writer. Later tests include two writer clients.
- Synchronous acknowledgment is the only required policy for the core milestone.
- Retain the full ordered history during this phase; the dataset stays small.
- In replication mode, reject COMPACT on both nodes until history/snapshot coordination
  is designed. Standalone COMPACT must still work.
- No primary promotion, automatic failover, multiple replicas, or competing primaries.
- Manual process restart/reconnect is sufficient; no service supervisor or elaborate
  retry scheduler is required.

Keeping complete history is a deliberate learning tradeoff: it makes catch-up
possible without introducing snapshots, log truncation, and snapshot offsets all
at once. Disabling replication-mode compaction is explicit behavior, not silent
loss of a replica's required history.

## Step 1: Send One Mutation And Get One Meaningful Reply

Before code, draw both processes and state what each owns. Use a dedicated replica
receiver and one test sender, rather than implementing the entire primary server
integration immediately.

Send one SET record. The receiver must:

1. Receive a complete framed message before executing anything.
2. Validate it using the agreed quoted-field rules and limits.
3. Append and flush its local persistence record; check failures.
4. Apply the mutation to its map.
5. Return one complete acknowledgment.

Check the value through the replica's read path, then restart it with the same log
and check again. Use explicit socket ownership, finite network waits, and joined
thread lifetimes. Reuse known send/receive patterns; a new networking framework
is unnecessary.

This is a transport-and-storage experiment. Numbered replay, reconnect recovery,
and the final two-node write guarantee are not complete yet. Do not claim them.

**Predict first:** if a message arrives in two receives, when may it execute? If
flushing fails, may the receiver send success?

## Step 2: Integrate The Primary And Define Client Success

Introduce primary and replica roles. Ordinary reads remain available on either
node; direct client mutations on the replica return a clear read-only error.
Define how peer traffic is distinguished from ordinary client traffic. Do not
accidentally allow a normal replica client connection to bypass write authority.

For a connected, ready replica, the intended mutation flow is:

```text
Primary validates client command
→ primary appends/flushes mutation and updates its map
→ primary sends that mutation to replica
→ replica appends/flushes and updates its map
→ replica sends ACK
→ primary receives and validates ACK
→ primary returns the command's normal success response
```

GET/EXISTS/SIZE do not produce mutation messages. DELETE of a missing key returns
its existing zero result and produces no mutation record. SET and successful
DELETE use the replicated path. A no-op response does not claim a new replicated
write occurred.

Specify two distinct failure results before choosing their wire spelling:

- **Known rejection before local application:** malformed command, read-only role,
  or replica already disconnected/not ready when the new mutation is admitted.
- **Outcome uncertain after local work:** local record may have been written, or
  replica delivery/ACK fails after local application. No normal success response;
  do not promise that the write is absent or roll it back blindly.

Before admitting another mutation, mark a failed peer session not ready and
resolve its pending history through reconnect/catch-up. Do not quietly fall back
to local-only successful writes. Reads may expose locally applied writes for
which the client never received success; document this explicitly.

Application retries can create new mutation records. We are not promising
exactly-once client requests. Even retrying DELETE can change its returned result:
if the original deletion happened, retrying finds the key absent.

**Predict first:** can the primary safely undo a write after a missing ACK? What
should a client infer from the outcome-uncertain error?

## Step 3: Make Order And Progress Recoverable

Add a sequence number to each mutation and an ACK tied to that number. One possible
internal message is `REPL 7 SET "color" "blue"` with a framing newline; an ACK could
be `ACK 7`. Choose exact syntax, size bounds, errors, and parser tests yourself.
The internal wrapper can make messages larger than an ordinary client request:
account for it instead of accidentally rejecting previously valid client commands.

Required behavior:

- Number actual mutations consecutively, beginning at 1 for a fresh history.
- Assign order in the same serialized operation that persists and applies the
  primary's change. Allocating a number then releasing ordering control before
  logging/sending is insufficient.
- With two writers, both nodes apply the same order, even for the same key.
- Persist sequence numbers with the mutations they identify. Replay reconstructs
  both the map and the last applied sequence after a normal restart.
- Avoid two independent log writes for “database data” and “replication progress”
  that can disagree after failure. A single numbered history serving both purposes
  is a simple starting design; document any alternative's consistency guarantee.
- Replica accepts n only when n is the next expected record, or a verified resend
  of an already applied record. Reject gaps and conflicting records.
- An old resend must not overwrite a newer value. Compare its contents with the
  retained history before acknowledging it without reapplying or relogging.
- Replica ACKs only after the required persistence/application steps. Primary
  checks the ACK number; unrelated/stale replies do not confirm its pending write.

The replication deployment also needs a stable history identity. Use a simple,
explicitly chosen identifier for this small experiment and retain it across
restarts. Fresh independent histories must not reuse it. Persist and check it
so a replica cannot attach to a different history merely because both reached
sequence 7. This is identity, not authentication or a leader-election term.

Use fresh replication-format files for this checkpoint; reject an incompatible
existing file with a clear error. Automatic migration is unnecessary. Reject
malformed complete records at startup. If an incomplete trailing record is ignored,
repair/remove that tail before accepting new appends, or refuse writes pending
repair: otherwise the next record could be joined to broken bytes. Stop successful
mutations after a persistence I/O failure until state/history is safely recovered.

**Make it click:** the same log can answer both “what is my state?” and “how far
through this specific history am I?” Separate metadata should not contradict it.

## Step 4: Reconnect And Catch Up

The replica reports its history identity and last applied sequence on reconnect.
A new empty replica can attach to the configured history and begin at sequence 0;
a nonempty replica must match the primary's identity and retained prefix.

```text
Primary has records 1–12
Replica reports applied through 9
Primary resends 10, 11, 12 in order
Replica applies/ACKs them
Both are through 12 → peer becomes ready for new writes
```

If the ACK for 10 was lost, the replica might already have 10. Verify the resend
against its history, ACK it, and leave its state and log unchanged. Current SET
and DELETE semantics alone do not solve this: an old SET resent after newer SETs
can restore an obsolete value.

Required behavior:

- Missing history is replayed before admitting new replicated mutations.
- New writes cannot overtake catch-up; serialize catch-up and mutation admission.
- Replica-ahead, wrong-identity, conflicting-prefix, missing-history, and sequence-gap
  cases fail clearly. Never silently merge histories or reset progress to hide them.
- A minimal correctness check can compare the replica's retained prefix with the
  primary's retained history; efficient checksums or log indexes are optional later.
- Restarting the primary preserves sequence numbering; restarting the replica
  preserves its applied position. Reconnecting does not manufacture new records
  for already persisted changes.
- Recovery is finite for the small test histories and fails visibly on timeout.
  Manual reconnection or restarting the disconnected process is enough initially.

Expose enough progress to test this: primary last local sequence, replica last
applied sequence, and ready/disconnected/catching-up status. The exact command or
output format is your choice; a dashboard is unnecessary. Distinguish last applied
from last ACK observed by the primary, since an ACK can be lost.

## Step 5: Exercise Partial Failure And Explain The Tradeoff

Use controlled test hooks or a small proxy if necessary to pause delivery or drop
an ACK at a precise point. Do not rely exclusively on sleeps and hope the failure
happens at the intended instruction.

Demonstrate these scenarios:

| Scenario | Expected baseline behavior |
| --- | --- |
| Replica unavailable before admission | New mutation rejected without applying it |
| Primary local append/flush fails | No success; no in-memory mutation for a known failed append/flush |
| Replica append/flush fails | No replica ACK; primary reports uncertain outcome if already applied locally |
| Replica applies record but ACK is lost | No confirmed client success; reconnect resolves progress without duplicate state/log effects |
| Replica restarts with its existing log | Recover state/position, catch up, then become ready |
| Primary restarts with its existing log | Recover history/next number, synchronize replica before new successful writes |
| Both restart with their respective logs | No loss of acknowledged state under the stated normal-restart contract |
| Primary is unavailable | Replica can serve its last applied reads; it does not accept new writes or promote itself |

No network timeout proves that the other process is dead. Keeping roles fixed
avoids two primaries accepting incompatible histories. If blocking replication
holds a mutation lock during a network wait, explain which requests it delays and
keep waits finite. This simple baseline may sacrifice throughput; required order
and ownership cannot be removed to make benchmark numbers better.

Document why this policy sacrifices successful writes during replica unavailability
in exchange for a stronger meaning of success. No automatic failover is required
to understand that tradeoff.

## Acceptance Checks For The Completed Core

Keep storage/parser tests separate from multi-process integration tests. All use
isolated files, chosen ports, finite waits, and reliable child-process cleanup.

- SET, overwrite, DELETE, and delete-then-SET converge in the same order.
- Missing DELETE and reads do not grow the mutation history.
- Quoted values, fragmented messages, multiple framed messages, malformed input,
  oversized input, and disconnect with partial input follow the documented protocol.
- Direct replica mutations and replication-mode COMPACT are rejected without
  modifying logs; standalone mode still passes its existing tests.
- Two writers cannot reorder persisted mutations, peer delivery, or state.
- Duplicate resends do not append again or undo newer state; conflicting resends,
  gaps, wrong history identity, and wrong ACKs fail visibly.
- Restart/reconnect cases recover state and progress; incomplete tails cannot
  corrupt later appends.
- Deliberate replica failure, dropped ACK, and local persistence failure produce
  the documented rejection/uncertainty behavior, not false success.
- Shutdown releases active peer I/O and joins every thread before destroying
  referenced queues, locks, logs, or store objects. Peer retry/catch-up state is bounded.

A long-lived peer connection must have explicit ownership and scheduling. Do not
accidentally consume all ordinary client workers and make the replica unable to
serve reads or shutdown. Choose the smallest separate peer-handling arrangement
that preserves those lifetimes; generic connection abstractions are optional.

## A Small Report, Not A New Benchmarking Project

Save a short `docs/replication-results.md` when the core behavior exists, containing:

- Role/startup commands, ports, separate file paths, history/message grammar.
- What successful replies guarantee, and what uncertain replies cannot tell us.
- A normal SET/GET/DELETE transcript showing both nodes.
- One restart/catch-up transcript and one lost-ACK or unavailable-replica example.
- One small comparison of standalone SET versus synchronous replicated SET with
  equivalent values, request counts, Release builds, and fresh histories.

Replica startup/catch-up belongs outside request timing unless that is the chosen
question. Do not compare a replicated success contract with local-only success and
claim replication code is inefficient solely because latency increases. Another
process must perform work before this stronger success response can be sent.

## What We Are Leaving For Later

These are later extensions, not prerequisites for the core checkpoint sequence:

- Configurable asynchronous/local-only acknowledgment policy and its lag/loss tests.
- Snapshots, replication-aware compaction, and efficient history retention/indexing.
- Automatic reconnect/backoff, many replicas, batching, and throughput optimization.
- Promotion/manual failover: requires a plan to stop/fence the old primary and
  determine which history may accept writes. Copying data alone is insufficient.
- Automatic leader election, quorum decisions, terms, consensus, and partition tolerance.
- Exactly-once client retries, transaction protocols, conflict resolution, and sharding.
- Authentication, TLS, public exposure, and stronger power-loss durability.

Manual failover and configurable acknowledgment are longer-term README topics.
Completing this core does not require implementing them yet.

## When This Phase Is Done

One primary and one replica preserve an ordered, restart-recoverable history;
normal successful mutations have been persisted/applied on both nodes; disconnects
and lost ACKs produce understandable behavior; catch-up restores a matching prefix;
and existing standalone storage/network guarantees still pass their checks.

You can explain the complete write flow, why numbering survives restart, why a
missing ACK is ambiguous, and why a replica is not a backup or automatic failover.
That is the learning milestone. More features should follow a new question.

## Your First Session

Before writing any replication implementation:

1. Draw the client, primary, replica, two maps, and two log files.
2. Trace one SET and mark exactly where the replica may ACK and client may see success.
3. Explain what GET should show on each node before and after that success.
4. Write an example framed mutation/ACK transcript with one malformed-message case.
5. Implement only Step 1: one mutation sent to a receiver with a meaningful ACK.

No need to master consensus before sending the first message. Start with the flow
you can explain, then add ordering and failure recovery as the next concrete needs.
