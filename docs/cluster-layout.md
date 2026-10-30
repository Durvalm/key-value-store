# Shared old/new layouts: first migration preparation step

A layout describes which groups exist, which ownership rule applies, and how to
find each group's primary, replica, expected history ID, and local log files.
The client and server can now use that same definition for live routing, node
startup, and ownership checks. The preview remains read-only. Data migration
and activation are still separate work.

## Try it

From the project directory:

```sh
cmake --build build-release
./build-release/kv_layout examples/layouts/old-modulo.layout examples/layouts/new-ring.layout color experiment-key-0 experiment-key-1
```

The example old layout describes two modulo groups. The new one describes three
ring groups with 64 positions each, new history IDs, new ports, and separate log
paths. The preview selects owners using the existing hashing/ring code:

```text
color: group-0 (:6380) -> group-0 (:6480)
experiment-key-0: group-1 (:6382) -> group-0 (:6480)
experiment-key-1: group-0 (:6380) -> group-0 (:6480)
```

These are calculations, not evidence that any values have moved. The command
opens only the two configuration files. It does not open log files or sockets,
create log directories, start servers, or modify the application's active routing.

## File format

```text
LAYOUT 1 fnv1a64-fmix64-ring-v1 64
GROUP "group-0" "new-group-0" 6480 6481 "data/new/primary-0.log" "data/new/replica-0.log"
GROUP "group-1" "new-group-1" 6482 6483 "data/new/primary-1.log" "data/new/replica-1.log"
```

The first line contains the file-format version, routing rule, and positions per
group. Supported routing rules are:

- `fnv1a64-mod-v1`: original FNV hash modulo the number of groups. Positions must
  be 1. Group line order defines the modulo index; the two-group example agrees
  with the current application's partition 0/1 routing.
- `fnv1a64-fmix64-ring-v1`: existing mixed hash and virtual-node naming rule.
  Group IDs determine positions; reordering GROUP lines does not change ownership.
  The positions count is part of the ownership definition, not a tuning knob to
  change on populated data without migration.

Each GROUP line contains, in order:

```text
GROUP "group ID" "history ID" primary-port replica-port "primary-log" "replica-log"
```

Group IDs identify owners. History IDs identify their replication histories; one
pair shares its ID, and separate pairs must have distinct IDs. The example source
IDs and paths are placeholders: they must be changed to describe the actual old
deployment before a future migration uses them. Merely loading a layout does not
check existing HISTORY/PARTITION headers, data, or server availability.

Paths may be absolute. Relative paths resolve against the layout file's directory,
not the shell's working directory. IDs and paths must be quoted, using the same
standard C++ quoted-string escaping convention as the store. Blank GROUP lines
are allowed; comments and additional fields are not supported.

This local milestone assumes loopback IPv4 endpoints. Each node needs a unique
port and log path. The parser validates IDs, distinct history IDs/ports/normalized
paths, port ranges, routing versions, and nonempty membership. Limits are 1–64
groups and 1–1024 ring positions per group, enough for the agreed local experiment
and bounded configuration sizes. Existing symlinks and lexical path aliases are
normalized, but this is not a complete migration path-safety check (for example,
future source/destination validation must also consider hard links).

## Code flow

1. `read_layout()` parses the file and resolves relative paths.
2. The `ClusterLayout` constructor validates the complete definition and builds
   the ring once when ring routing is selected. Group definitions are exposed
   through const references, avoiding accidental mutation of the cached mapping.
3. `owner(key)` applies modulo or ring lookup, returning the actual group
   definition including its endpoint and paths.
4. `kv_layout` loads old and new definitions and prints both owners for each key.

The ring implementation is reused; there is no second hashing algorithm hidden
inside the parser. `group(id)` provides an explicit lookup for node startup later.

## Run actual servers and clients from a layout

```sh
./build-release/kv_client --layout <file>
./build-release/kv_server --layout <file> <group-id> <primary|replica> [idle-timeout-seconds]
```

Server startup selects its port, log path, expected history ID and ownership rule
from the selected group. Start each replica before its primary. Layout-managed
nodes always use numbered replicated histories. Both members of a pair must use
layout mode. The client loads its layout once, chooses `owner(key).primary_port`,
and uses the existing one-connection-per-request flow. HELP/EXIT remain local;
cluster-wide SIZE/COMPACT remain unsupported. Direct requests to the wrong owner
return WRONG_PARTITION, and direct replica writes remain read-only.

To try the example three-group ring, first create its log directory:

```sh
mkdir -p examples/layouts/data/new
```

Run each following server command in its own terminal; start all replicas first:

```sh
./build-release/kv_server --layout examples/layouts/new-ring.layout group-0 replica
./build-release/kv_server --layout examples/layouts/new-ring.layout group-1 replica
./build-release/kv_server --layout examples/layouts/new-ring.layout group-2 replica
```

Then the primaries, also each in its own terminal:

```sh
./build-release/kv_server --layout examples/layouts/new-ring.layout group-0 primary
./build-release/kv_server --layout examples/layouts/new-ring.layout group-1 primary
./build-release/kv_server --layout examples/layouts/new-ring.layout group-2 primary
```

Finally:

```sh
./build-release/kv_client --layout examples/layouts/new-ring.layout
```

```text
SET "color" "blue"
SET "full name" "Durval"
SET "experiment-key-6" "third group"
GET "color"
GET "full name"
GET "experiment-key-6"
EXIT
```

These three keys select group-0, group-1 and group-2 respectively. The example
uses ports 6480/6481, 6482/6483 and 6484/6485. Reuse the same configuration and
files on restart. Generated logs must stay untracked; the example paths point
inside this repository's examples directory.

## Persisted ownership and peer agreement

Fresh layout-managed logs begin with a complete canonical ownership header:

```text
OWNERSHIP "group-0" fnv1a64-fmix64-ring-v1 64 "group-0" "group-1" "group-2"
HISTORY new-group-0
1 SET "color" "blue"
```

The header includes this group, routing algorithm, position count and membership.
Ring membership is sorted by ID for this description, so reordering GROUP lines
is harmless. Modulo membership order determines indices and must be retained.
Ports and paths are excluded because relocating an endpoint does not itself
change ownership. The expected history ID is still checked separately.

A saved header must match startup configuration before any mutations are replayed
or new records appended. A changed routing rule, position count, membership or
group cannot silently reinterpret an existing layout log. Replayed and peer
mutations still pass the same ownership checks as ordinary writes.

Layout-managed peers also exchange this ownership header as an additional quoted
field in their REPLICA handshake. A mismatch fails primary startup before any
new client writes; rejecting it does not consume the replica's one peer session.
The bounded layout handshake can be larger than an ordinary command (up to 16 KiB,
covering the layout's 64 groups/128-byte ID limits). Ordinary requests retain the
4096-byte limit. These identity checks detect configuration mistakes; they do not
add authentication or automatic failover.

## Existing two-partition deployments

Original single-server and `--cluster` client commands and positional server
arguments still work. Existing PARTITION logs can also be opened through a layout
when it describes the equivalent two-group modulo mapping: correct group index,
expected history ID, and the existing file paths. Both nodes must start through
that layout; their existing log prefix remains unchanged.

To do this, update the old-layout example to use the actual paths and HISTORY IDs
from your deployment. Its placeholder `old-group-0`/`old-group-1` IDs are not
automatically substituted. A ring layout cannot open those modulo logs: use fresh
destination files and the upcoming migration tool instead of changing the routing
field in place. New layout-managed logs also cannot bypass their saved identity
by reopening them through legacy server arguments.

Configuration is loaded once per process. Editing a file is not a membership
change protocol, and it does not reconfigure running servers. Offline migration
and a coordinated switch remain necessary before clients use a new ownership map.

## What is still next

The [offline source snapshot](source-snapshot.md) now reads and validates stopped
source groups, compares complete primary/replica histories, and exports current
values once per primary while retaining the source logs. File-backed stores hold
lifetime process locks so snapshot collection refuses running nodes.

Transfer into fresh destination groups, exact destination verification and safe
activation remain next. Traffic must stay paused and original logs intact
throughout that offline operation. A snapshot alone is not a completed migration.

## Verification

```sh
ctest --test-dir build-release --output-on-failure
```

Unit tests cover parsing, ownership, canonical identity, mismatched restart
configuration, and legacy modulo compatibility. A separate integration suite
runs three ring groups through the real client, verifies primary/replica data,
wrong-owner rejection, replica read-only behavior, and full restart with reordered
ring membership. It also exercises an oversized valid ownership handshake,
rejected peer layout mismatch without consuming the session, and a running 7A
modulo deployment reopened through an equivalent layout. Reading/previewing a
layout alone must not create the referenced log directories.
