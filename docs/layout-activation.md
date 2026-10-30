# Controlled layout activation

Transfer puts data in its new home. Activation publishes the layout that tells
clients to use that home. This is the final configuration switch in our offline
migration, with traffic paused throughout.

## What the command does

```sh
./build-release/kv_activate source.layout destination.layout active.layout
```

1. Load and validate both layouts. The active file and its sidecar lock must be
   separate from input layouts, database logs and database lock files.
2. Lock `active.layout.lock`, preventing two cooperating activators from publishing
   simultaneously. If an active file exists, it must describe the source deployment.
   An absent active file is allowed for the first migration using this convention.
3. Lock all stopped sources and collect their verified live state.
4. Verify every destination value, count, identity and stable sequence on both
   copies, using the existing exact verifier. Also reject primaries with an import
   reservation or a recorded replication failure.
5. Write the complete destination configuration to an exclusive temporary file in
   the active file's directory. Flush and close it, then rename it over the active
   file. Retain source locks until publication finishes.

The published file uses the existing LAYOUT/GROUP format. Log paths are absolute,
so placing this file in another directory does not change which files it names.
Servers already running with the candidate layout need no configuration change:
the active file describes the same deployment. Clients can use the existing
`--layout` option; there is no new routing mechanism.

## Operator procedure

Use layouts describing your actual deployment. Do not substitute the example
source layout for existing logs with different history IDs or ownership.

1. **Stop every normal client.** Recover/synchronize old primary/replica pairs if
   needed, then stop all source nodes. Keep their original logs.
2. Start fresh candidate replicas, then candidate primaries, using the destination
   layout. For each group, use separate terminals:

   ```sh
   ./build-release/kv_server --layout destination.layout group-0 replica
   ./build-release/kv_server --layout destination.layout group-0 primary
   ```

   Repeat for every destination group, substituting its group ID.
3. Copy the live dataset:

   ```sh
   ./build-release/kv_transfer source.layout destination.layout
   ```

   If transfer fails, do not activate. Inspect the failure and rebuild incomplete
   candidates with fresh files/histories before retrying from the intact sources.
4. Still with traffic paused, stop candidate primaries, then replicas. Restart
   replicas first, then primaries, using the same destination layout and saved logs.
   This clears the transfer's in-memory import reservations and exercises normal
   replay and startup catch-up. Do not restart the old source nodes.
5. Activate:

   ```sh
   ./build-release/kv_activate source.layout destination.layout active.layout
   ```

   This repeats verification now; it does not trust an earlier `kv_verify` result.
   `ERROR ACTIVATION_NOT_READY` means a candidate primary still has an import
   reservation or recorded replication failure. Check/restart the candidate pairs
   while traffic remains paused, then retry verification/activation.
6. On success, start **all** clients using the published file, then resume traffic:

   ```sh
   ./build-release/kv_client --layout active.layout
   ```

   Do not start clients with the old layout or use legacy direct endpoints to
   continue writing to the old deployment. Keep old sources stopped and retain
   their logs until the migration is accepted.

Clients read layouts once at startup. Publishing does not update already-running
clients, terminate old ones, start nodes or resume traffic. Those are explicit
operator actions in this learning milestone.

## Why a temporary file and rename?

Writing directly into `active.layout` could leave a half-written configuration
if the process stopped during the write. Instead:

```text
active.layout = complete old layout
                     |
write + flush complete new layout to a separate temporary file
                     |
rename temporary file over active.layout  ← publication point
                     |
active.layout = complete new layout
```

A same-directory POSIX rename changes the file name atomically: a reader opening
it sees the old complete file or the new complete file. It never sees our partial
write. `mkstemp` creates a unique temporary file without truncating an existing
file. The existing `LogLock` supplies the process lock; closing its descriptor,
including on process death, releases that lock.

This is atomic file publication, not an atomic update of every client process.
The paused-traffic procedure supplies that deployment-wide coordination.

## Failures and recovery

Before the rename, a rejected check or handled write error leaves the previous
active file unchanged (or absent on first publication). Handled publication errors
remove their temporary file. Source and destination database logs are never edited
by this command. It does not delete candidate data or automatically roll back.

If killed during temporary-file creation, an unused `active.layout.tmp-*` file may
remain; it is never treated as active. Inspect/remove such leftovers only after the
activator has stopped. The persistent `.lock` sidecar is normal; do not delete it
while an activator is running.

A crash around the rename may occur after publication but before the success
message. Inspect the active layout before deciding what happened. A repeat using
an obsolete source is rejected once the active layout names the new deployment.
Keep traffic paused and use `kv_verify` again if needed. Never switch back to old
logs after accepting new writes without another migration: those old logs are stale.

The temporary file is fsynced before rename. We do not promise power-loss durability
of the rename (the parent directory is not fsynced), nor do the database journals
currently promise power-loss durability. The atomicity guarantee here covers normal
file visibility and process interruption, not a durable distributed transaction.

The operator must prevent destination writes and configuration edits throughout
verification/publication. There is no automatic fencing, authentication, online
migration, distributed agreement or rollback orchestration.

## Evidence and important code

- `layout_activation.cpp`: separation checks, active-file lock, fresh verification,
  absolute-path serialization and atomic publication.
- `destination_verification.cpp`: `VERIFY_ACTIVATE` uses the same exact read checks.
- `server_main.cpp`: activation readiness checks shared primary flags under the
  replication mutex. The control message does not change server state.
- `kv_activate`: CLI entry point only; it does not manage other processes.

```sh
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Tests exercise modulo 2 → ring 3 and ring 3 → ring 4 through transfer, rejection
before candidate restart, restart, activation, client reads and a replicated write.
They check source preservation, wrong data, mismatched active state, output aliases,
relative-path relocation, first publication, concurrent activators, and interruption
before publication followed by retry. Both moved/unmoved data and deleted/overwritten
source keys are included in the migration scenarios.
