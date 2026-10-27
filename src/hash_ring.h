#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Experimental ownership only. The live servers still use partition_for_key().
struct RingEntry
{
    std::uint64_t position;
    std::string group_id;
};

// One stable FNV-1a position per group. Sort by position, then ID to break ties.
std::vector<RingEntry> build_ring(const std::vector<std::string> &group_ids);

// Entries must be sorted as above. Search forward, wrapping to the first entry.
std::string group_for_position(const std::vector<RingEntry> &ring, std::uint64_t position);
std::string group_for_key(const std::vector<RingEntry> &ring, const std::string &key);
