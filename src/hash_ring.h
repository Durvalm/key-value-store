#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared ring ownership for layout mode; legacy routing still uses partition_for_key().
struct RingEntry
{
    std::uint64_t position;
    std::string group_id;
};

// Experimental ring hash v1: FNV-1a 64-bit followed by MurmurHash3's fmix64.
// Used for BOTH keys and group IDs. The application's modulo hash is unchanged.
std::uint64_t ring_hash(const std::string &text);

// Position 0 hashes the bare ID; extra positions hash ID + '#' + decimal index.
// Every entry returns the real group ID. Sort by position, then ID to break ties.
std::vector<RingEntry> build_ring(const std::vector<std::string> &group_ids,
                                std::size_t positions_per_group = 1);

// Entries must be sorted as above. Search forward, wrapping to the first entry.
std::string group_for_position(const std::vector<RingEntry> &ring, std::uint64_t position);
std::string group_for_key(const std::vector<RingEntry> &ring, const std::string &key);
