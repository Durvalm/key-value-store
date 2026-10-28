# First ownership experiment (7B)

Run from the project directory:

```sh
cmake --build build-release
./build-release/kv_partition_experiment
```

This program only calculates ownership. It opens no sockets, reads no store logs,
and moves no data. The live client/server still use the original two-partition
modulo rule. Virtual nodes are now part of this experiment; migration is not implemented.

## Read the code in this order

1. `src/hash_ring.h`: a RingEntry contains a position and a group ID. A ring is
   just a sorted vector of these entries, not another process or network.
2. `build_ring` in `src/hash_ring.cpp`: validate group IDs, generate the requested
   positions per group, then sort by position. Every entry retains the real group
   ID. Existing token positions remain fixed when a new group is added.
3. `group_for_position`: loop through the sorted vector; return the first entry
   whose position is at least the requested number. If none qualifies, return
   the first entry. That last return is the wrap-around.
4. `group_for_key`: hash the key and call that same lookup.
5. `src/partition_experiment.cpp`: compare ownership before and after insertion.

Lookup uses a linear scan for this small experiment. A binary search is optional
later. `ring_hash` now runs the existing FNV-1a helper followed by the standard
MurmurHash3 `fmix64` finalizer. Both keys and group IDs use this mixed 64-bit hash.
The example numbers 20, 45 and 70 remain manually chosen teaching positions,
not the actual hashes of the group IDs.

At an exact position, that entry owns the key. Position collisions are sorted by
group ID as a deterministic tie-break: only the first entry at that position owns
an interval. Collisions do not mean duplicate copies. Duplicate group IDs and
empty membership/IDs are rejected. Callers of position lookup must provide a ring
sorted by position and then ID; build_ring produces that ordering.

## Small example: how lookup works

```text
Before: 20 → group-0,                  70 → group-1
After:  20 → group-0, 45 → group-2,     70 → group-1
```

Captured output:

```text
key position 12: group-0 -> group-0
key position 33: group-1 -> group-2
key position 60: group-1 -> group-1
key position 85: group-0 -> group-0
Toy ring: 25/100 positions change owners (21 through 45).
```

A key at 33 used to find 70 first; now it finds 45 first. A key at 85 still wraps
to 20. No keys move between the existing groups when inserting the new entry:
a changed owner is always the newly added group. This describes ownership changes,
not actual data transfers.

## Baseline before mixing: balance is not guaranteed

Captured 2026-10-27, using exactly 10,000 keys named `experiment-key-0` through
`experiment-key-9999`, with the same FNV hash for both algorithms.
Modulo uses the ordered group list group-0, group-1, then appends group-2.

The actual sorted group positions are:

```text
group-2: 79347867365069971
group-1: 79348966876698182
group-0: 79350066388326393
```

Those numbers are extremely close in a space spanning 0 through 2^64−1.

| Strategy | group-0 keys | group-1 keys | group-2 keys |
| --- | ---: | ---: | ---: |
| Modulo, two groups | 5,000 | 5,000 | Not present |
| Modulo, three groups | 3,333 | 3,333 | 3,334 |
| Ring, two groups | 0 | 10,000 | Not present |
| Ring, three groups | 0 | 0 | 10,000 |

Modulo changed 6,668 owners (66.68%). This basic ring changed 10,000 (100%).
These are measured counts for this dataset, not general expected percentages.

Why? With nearly adjacent positions, one group owns almost the entire wrap-around
interval. Before insertion, group-1 is first in sorted order. Group-2 becomes the
new first entry and takes that huge interval when inserted. The algorithm changes
only the new group's interval, but that interval can be enormous.

This does not disprove consistent hashing's value. It shows that a single point
per group and this hash/identifier combination are poor placement inputs. A hash
that works for modulo routing is not automatically suitable for ring placement:
modulo uses remainders, whereas ring ownership depends on gaps across the full
hash space. Neither less movement nor good balance is guaranteed for every
particular ring and dataset.

## Added mixing step: same keys, more spread, still unequal intervals

The ring now uses the fixed experimental rule `fnv1a64-fmix64-v1`:

```text
Input string → original FNV-1a 64-bit hash → fmix64 → ring position
```

The finalizer is Austin Appleby's public-domain
[fmix64 implementation](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp).
It applies XOR/right-shift operations and two fixed odd multipliers, with unsigned
64-bit wrap-around. This spreads differences in the FNV result across its bits;
it does not choose random positions, enforce equal loads, remove pre-existing FNV
collisions, or provide cryptographic protection. This composition is not the full
MurmurHash3 string-hashing algorithm.

The original `stable_key_hash` and application routing remain unchanged. No new
dependency or live hash-selection option was added. Only experimental ring
construction and key lookup call `ring_hash`. Its exact outputs
are fixed by golden tests; changing the algorithm changes ownership.

Captured 2026-10-27, rerunning the exact same group IDs and 10,000 keys:

```text
group-2: 1710278319524499557
group-0: 3525019007807396484
group-1: 16342283995423876884
```

| Strategy | group-0 keys | group-1 keys | group-2 keys |
| --- | ---: | ---: | ---: |
| Original modulo, two groups | 5,000 | 5,000 | Not present |
| Original modulo, three groups | 3,333 | 3,333 | 3,334 |
| Mixed ring, two groups | 3,119 | 6,881 | Not present |
| Mixed ring, three groups | 999 | 6,881 | 2,120 |

Modulo still changes 6,668 owners (66.68%). The mixed ring changes 2,120 owners
(21.20%), all to the new group. Group-1 keeps its 6,881 keys; group-0 gives 2,120
of its 3,119 keys to group-2. These percentages describe this dataset and these
IDs, not a general movement guarantee. The experiment executable prints the
current mixed result; the original raw-FNV baseline is preserved above.

This fixes the observed near-identical positions without achieving equal balance:
group-1 still owns about 69% of sampled keys because the gap before its position
is large. Key counts measure storage ownership, not request popularity or CPU load.

## Virtual nodes: multiple entries for the same group

`build_ring(group_ids, positions_per_group)` now accepts a positive position count;
it defaults to 1, preserving the previous experiment's output and ownership.
The lookup functions and RingEntry structure are unchanged.

```cpp
const auto ring = build_ring({"group-0", "group-1", "group-2"}, 64);
```

This creates 192 entries, not 192 servers. For each real group, index 0 keeps the
existing `ring_hash(id)` position. Extra positions use:

```text
index 0: ring_hash("group-0")   → entry pointing to group-0
index 1: ring_hash("group-0#1") → entry pointing to group-0
index 2: ring_hash("group-0#2") → entry pointing to group-0
...
index 63: ring_hash("group-0#63") → entry pointing to group-0
```

The suffix is only the input for generating a position. The returned owner is
always the real group ID. Multiple sorted entries let a group own several smaller
intervals instead of depending on the size of one gap. Zero positions are rejected.
Position collisions retain the existing position-then-group-ID tie rule.

The experiment compares 1 against 64 positions per group using the same IDs, mixed
hash and 10,000 keys. Sixty-four is a fixed teaching setting, not a tuned optimum.
Captured 2026-10-27:

| Positions per group | Groups | group-0 keys | group-1 keys | group-2 keys |
| --- | ---: | ---: | ---: | ---: |
| 1 | 2 | 3,119 | 6,881 | Not present |
| 1 | 3 | 999 | 6,881 | 2,120 |
| 64 | 2 | 5,424 | 4,576 | Not present |
| 64 | 3 | 3,277 | 3,046 | 3,677 |

With 64 positions, 3,677 keys change owners when group-2 is added: **36.77%**, all
to group-2. This is more movement than the single-position ring's 21.20%, because
the new group now receives a larger, more balanced share. Lower movement alone
is not the goal if it leaves one group owning most keys. Original modulo changes
66.68% of owners, including movement between existing groups.

These counts show better key balance for this dataset, not guaranteed perfect
balance or balanced request traffic. The existing maximum group share after
adding a third group falls from 68.81% with one position to 36.77% with 64.

## What to do next

We now have an ownership rule to evaluate for offline migration. Before connecting
it to servers, the chosen hash, group IDs, token naming and position count must be
part of one fixed deployment definition. All routers and nodes must agree.
Changing the position count can change ownership even though old token positions
remain present; it is not a safe live configuration toggle.

The application still uses modulo routing and its existing log headers. Neither
this experiment nor changing a group list moves stored values. Offline migration
must build and verify fresh destination histories from the original live state.

## Checks

```sh
ctest --test-dir build-release --output-on-failure
```

The ring test fixes full mixed-hash outputs, including UTF-8 and embedded NUL
bytes, and checks known group/key mappings. It also covers exact boundaries,
wrap-around including the maximum
64-bit value, insertion/removal locality, input ordering, single-group lookup,
invalid membership and the collision tie rule. Virtual-node tests check counts,
real-group identity, fixed token positions, membership-order independence, adding
and removing groups, and zero-count rejection. Existing hash vectors and live
store/network/replication tests also continue to pass.
