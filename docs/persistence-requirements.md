# Part 2: Persistence Requirements

## Purpose

Add disk persistence to the in-memory key-value store so acknowledged mutations survive a normal process restart.

This document defines required behavior, guarantees, failure policies, and acceptance tests. It intentionally does not prescribe class names, method signatures, or implementation algorithms. Those are part of the learning exercise.

## Learning Objectives

By completing this phase, be able to explain and demonstrate:

- The difference between in-memory state and durable history.
- Why an append-only log makes writes simple but recovery more expensive.
- Why a mutation must be logged before it is applied to memory.
- How serialization converts operations into stored records.
- How replay reconstructs current state from historical operations.
- How partial writes and malformed records differ.
- How compaction removes obsolete history without changing current state.
- Which failures the current durability guarantee does and does not cover.

## Existing Behavior to Preserve

The current commands must continue to behave as they do now:

- `SET <key> <value>` inserts or overwrites a value.
- `GET <key>` returns the current value or `(nil)`.
- `DELETE <key>` returns `1` when a key was removed and `0` otherwise.
- `EXISTS <key>` reports whether a key currently exists.
- `SIZE` reports the number of live keys.
- `HELP` and `EXIT` continue to work.
- Existing in-memory unit tests continue to pass.

## Phase 2A: Append and Recover

### P1. Configurable Storage File

- The program must be able to open a caller-selected persistence file.
- Opening a missing file must create an empty database and a new log.
- Opening an empty file must create an empty in-memory state.
- Runtime database files must not be committed to Git.

The exact CLI syntax and default path are design decisions.

### P2. Mutation Records

- Every successful `SET` must append one record containing the operation, key, and value.
- Every successful `DELETE` must append one delete record.
- Deleting a missing key must return `0` and must not append a mutation.
- `GET`, `EXISTS`, `SIZE`, `HELP`, and startup itself must not append records.
- Repeated updates to one key must append new records rather than editing earlier records.

The first format must be human-readable text. It must unambiguously represent:

- The operation type.
- Keys.
- Values containing spaces.
- Quotes or escaping according to a documented rule.
- The boundary between complete records.

The precise record grammar is a design decision and must be documented before implementation.

### P3. Write Ordering

For each mutation, the system must:

1. Construct the log record.
2. Append the complete record.
3. Verify the stream did not fail.
4. Flush the stream.
5. Apply the mutation to the in-memory store.
6. Report success to the caller.

If appending or flushing fails:

- The in-memory mutation must not occur.
- The CLI must not print `OK` or otherwise report success.
- The failure must be visible to the caller.

The mechanism used to represent an I/O failure is a design decision.

### P4. Startup Replay

When opening an existing log, the system must read records in file order and reconstruct the in-memory state.

Replay must support:

- New keys.
- Overwritten values.
- Deleted keys.
- A key being deleted and later created again.
- Multiple independent keys.
- Values containing spaces.

Replay must not append the replayed records back into the log. Opening and immediately closing a database without mutations must leave the log unchanged.

### P5. Recovery Policy

For this phase, use the following policy:

- Missing file: create it and continue with an empty store.
- Empty file: continue with an empty store.
- Valid records: replay them.
- Blank lines: either ignore or reject them, but document and test the choice.
- Incomplete final record: ignore it and produce a visible warning.
- Malformed complete record in the middle: stop startup with a clear error.
- Unknown operation: stop startup with a clear error.

The system must never silently interpret a malformed complete record as valid data.

## Phase 2B: Manual Compaction

Add a manual `COMPACT` command after Phase 2A is correct.

Compaction must:

- Produce a new log representing only the current live key-value state.
- Remove overwritten values and deleted keys from the rewritten history.
- Preserve exactly the same observable database state.
- Write to a temporary file rather than modifying the active log in place.
- Verify and flush the temporary file before replacement.
- Replace the old log atomically when supported by the filesystem.
- Reopen the compacted log for future appends.
- Leave the original recoverable if writing the temporary file fails.

Automatic or background compaction is not required.

## Phase 2C: Stronger Recovery

Only begin this phase after append, replay, and manual compaction are complete.

Investigate and document:

- Checksums for detecting record corruption.
- A version field for future record-format changes.
- Fixed-size or length-prefixed record headers.
- The difference between stream `flush()` and operating-system `fsync`.
- The durability and latency cost of syncing every write.
- Batching or group commit.
- A threshold for automatic compaction.

Implementation of every item is not required yet. Select improvements based on observed failure modes and measurements.

## Durability Guarantee

At the end of Phase 2A, the promised guarantee is:

> A mutation reported as successful survives a normal process exit and restart, provided the persistence file remains available and uncorrupted.

The implementation must not yet claim guaranteed survival from:

- Power loss.
- Operating-system crash.
- Disk hardware failure.
- Filesystem corruption.
- Concurrent writes from multiple processes.

Stronger claims require explicit implementation and testing in Phase 2C or later.

## Required Tests

All persistence tests must use isolated temporary paths. They must never read or modify a personal database file.

### Append and Replay

- A missing file starts empty and can accept writes.
- A stored value survives destruction and reopening of the store.
- An overwritten value recovers as the newest value.
- A deletion remains deleted after restart.
- Delete followed by a later set recovers the later value.
- Multiple keys recover independently.
- Values containing spaces round-trip exactly.
- A failed deletion does not append a record.
- Read-only operations do not change the log.
- Replay itself does not change the log.

### Recovery Errors

- An empty file starts successfully.
- An incomplete final record follows the documented recovery policy.
- A malformed middle record prevents startup.
- An unknown operation prevents startup.
- A simulated append failure does not update in-memory state.

### Compaction

- Compaction preserves every live key and value.
- Compaction does not restore deleted keys.
- Reopening a compacted log produces the same state.
- Compaction reduces a log containing obsolete history.
- Failure before replacement leaves the original log recoverable.

## Non-Goals

Do not add these during the initial persistence phase:

- Networking.
- Threads or concurrent clients.
- Multiple writer processes.
- Replication.
- Transactions across multiple keys.
- Binary protocol compatibility.
- SSTables, LSM trees, or B-trees.
- Background compaction.
- Production-grade encryption or authentication.

## Implementation Checkpoints

Complete these checkpoints in order. Do not implement the entire phase in one pass.

### Checkpoint 1: Record Round Trip

- Define the record semantics and text grammar in `docs/learning-log.md`.
- Write one mutation record to a temporary file.
- Read it back.
- Prove that the operation, key, and value match exactly.

### Checkpoint 2: Replay

- Read a sequence of records.
- Apply them to a fresh in-memory store.
- Prove that overwrites and deletes produce the expected final state.

### Checkpoint 3: Persistent Mutations

- Connect successful runtime mutations to the log.
- Preserve the required write ordering.
- Prove that reopening reconstructs the state.

### Checkpoint 4: Recovery Errors

- Implement the incomplete-tail policy.
- Reject malformed complete records.
- Test failed writes without changing memory.

### Checkpoint 5: Compaction

- Add manual compaction using a temporary file.
- Prove state equivalence before and after compaction.
- Prove restart recovery from the compacted log.

## Learning Rules

Before each checkpoint:

1. Write the proposed data flow and guarantees in `docs/learning-log.md`.
2. Choose the public types and method signatures yourself.
3. Predict at least two failure cases.
4. Write or outline the acceptance tests.
5. Implement the smallest behavior that can pass those tests.

When asking for help, bring the attempted design, compiler error, failed test, or observed behavior. Reviews should explain tradeoffs and identify incorrect guarantees without replacing the entire checkpoint implementation.

## Definition of Done

Persistence is complete when:

- All Phase 2A and Phase 2B requirements are implemented.
- Existing in-memory tests still pass.
- All required append, replay, recovery, and compaction tests pass.
- The file format and recovery policy are documented.
- The durability guarantee is stated without claiming `fsync`-level durability.
- A fresh clone can build the project and run the complete test suite.
- The implementation can be explained from record creation through restart recovery.
