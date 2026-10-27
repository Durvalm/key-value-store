#include "hash_ring.h"
#include "partition.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    int failures = 0;
    void expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    template<typename Operation>
    void expect_invalid(Operation operation, const std::string &message)
    {
        try
        {
            operation();
            expect(false, message);
        }
        catch (const std::invalid_argument &)
        {
        }
    }
}

int main()
{
    const std::vector<RingEntry> before{{20, "group-0"}, {70, "group-1"}};
    const std::vector<RingEntry> after{{20, "group-0"}, {45, "group-2"}, {70, "group-1"}};
    for (std::uint64_t position : {0, 12, 20, 71, 85, 99})
        expect(group_for_position(before, position) == "group-0", "first entry and wrap-around");
    for (std::uint64_t position : {21, 33, 60, 70})
        expect(group_for_position(before, position) == "group-1", "first position at or after key");
    expect(group_for_position(after, 45) == "group-2", "exact position belongs to that entry");
    expect(group_for_position(after, 46) == "group-1", "next position belongs to next entry");
    expect(group_for_position(after, std::numeric_limits<std::uint64_t>::max()) == "group-0",
           "maximum hash wraps without overflow");
    for (std::uint64_t position = 0; position < 100; ++position)
    {
        const auto old_owner = group_for_position(before, position);
        const auto new_owner = group_for_position(after, position);
        expect((old_owner != new_owner) == (position > 20 && position <= 45),
               "insertion changes only the new group's interval");
    }
    // Removing group-2 restores old ownership and does not move other owners.
    for (std::uint64_t position = 0; position < 100; ++position)
        if (group_for_position(after, position) != "group-2")
            expect(group_for_position(after, position) == group_for_position(before, position),
                   "removal changes only removed group's keys");

    const auto ring = build_ring({"group-0", "group-1", "group-2"});
    const auto reordered = build_ring({"group-2", "group-0", "group-1"});
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        expect(ring[i].position == stable_key_hash(ring[i].group_id), "positions hash stable IDs");
        expect(ring[i].position == reordered[i].position && ring[i].group_id == reordered[i].group_id,
               "configuration order does not change ring");
    }
    const auto smaller = build_ring({"group-0", "group-1"});
    for (std::size_t i = 0; i < 10000; ++i)
    {
        const auto key = "experiment-key-" + std::to_string(i);
        const auto old_owner = group_for_key(smaller, key);
        const auto new_owner = group_for_key(ring, key);
        expect(new_owner == old_owner || new_owner == "group-2", "no movement between old owners on insertion");
        expect(new_owner == group_for_key(reordered, key), "key mapping independent of input order");
    }
    const auto single = build_ring({"only-group"});
    expect(group_for_key(single, "any key") == "only-group", "one group owns every key");
    const std::vector<RingEntry> tied{{20, "a"}, {20, "b"}, {70, "c"}};
    expect(group_for_position(tied, 20) == "a", "collision uses first ID in sorted order");
    expect(group_for_position(tied, 21) == "c", "colliding entries do not create a new interval");
    expect_invalid([] { build_ring({}); }, "reject empty membership");
    expect_invalid([] { build_ring({""}); }, "reject empty ID");
    expect_invalid([] { build_ring({"same", "same"}); }, "reject duplicate ID");
    expect_invalid([] { group_for_position({}, 33); }, "reject empty lookup");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "All hash ring tests passed.\n";
    return EXIT_SUCCESS;
}
