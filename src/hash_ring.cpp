#include "hash_ring.h"
#include "partition.h"

#include <algorithm>
#include <stdexcept>

std::uint64_t ring_hash(const std::string &text)
{
    std::uint64_t hash = stable_key_hash(text);
    // fmix64 mixes differences across the output bits. Unsigned products wrap.
    // Austin Appleby, public domain: github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

std::vector<RingEntry> build_ring(const std::vector<std::string> &group_ids,
                                std::size_t positions_per_group)
{
    if (group_ids.empty())
        throw std::invalid_argument("A ring needs at least one group");
    if (positions_per_group == 0)
        throw std::invalid_argument("Each group needs at least one position");

    std::vector<RingEntry> ring;
    for (const std::string &id : group_ids)
    {
        if (id.empty())
            throw std::invalid_argument("Group ID must not be empty");
        for (const RingEntry &entry : ring)
            if (entry.group_id == id)
                throw std::invalid_argument("Duplicate group ID");
        for (std::size_t index = 0; index < positions_per_group; ++index)
        {
            // Keep the original position so increasing the count only adds entries.
            const std::string token = index == 0 ? id : id + '#' + std::to_string(index);
            ring.push_back({ring_hash(token), id});
        }
    }
    std::sort(ring.begin(), ring.end(), [](const RingEntry &a, const RingEntry &b)
    {
        if (a.position == b.position)
            return a.group_id < b.group_id;
        return a.position < b.position;
    });
    return ring;
}

std::string group_for_position(const std::vector<RingEntry> &ring, std::uint64_t position)
{
    if (ring.empty())
        throw std::invalid_argument("Cannot look up an empty ring");
    for (const RingEntry &entry : ring)
        if (entry.position >= position)
            return entry.group_id;
    return ring.front().group_id;
}

std::string group_for_key(const std::vector<RingEntry> &ring, const std::string &key)
{
    return group_for_position(ring, ring_hash(key));
}
