#include "hash_ring.h"
#include "partition.h"

#include <algorithm>
#include <stdexcept>

std::vector<RingEntry> build_ring(const std::vector<std::string> &group_ids)
{
    if (group_ids.empty())
        throw std::invalid_argument("A ring needs at least one group");

    std::vector<RingEntry> ring;
    for (const std::string &id : group_ids)
    {
        if (id.empty())
            throw std::invalid_argument("Group ID must not be empty");
        for (const RingEntry &entry : ring)
            if (entry.group_id == id)
                throw std::invalid_argument("Duplicate group ID");
        ring.push_back({stable_key_hash(id), id});
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
    return group_for_position(ring, stable_key_hash(key));
}
