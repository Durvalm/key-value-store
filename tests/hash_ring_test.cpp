#include "hash_ring.h"

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
    struct HashCase
    {
        std::string text;
        std::uint64_t expected;
    };
    // Fixed vectors from a separate Python FNV + fmix64 calculation.
    const HashCase hash_cases[] = {
        {"", 0xefd01f60ba992926ULL},
        {"a", 0x82a2a958a9bece5bULL},
        {"group-0", 0x30eb621c7adc1a84ULL},
        {"group-1", 0xe2cb74b56dcaa714ULL},
        {"group-2", 0x17bc2111f64f5065ULL},
        {"group-0#1", 0x833b40f2c4518018ULL},
        {"group-0#2", 0x420c5409217f812aULL},
        {"group-1#1", 0x76258375e8cc202cULL},
        {"color", 0x9ddbe96d41032da8ULL},
        {"full name", 0x915d7ac3e2af53f2ULL},
        {"caf\xc3\xa9", 0xf50b1f8e2c0682e6ULL},
        {std::string("prefix\0suffix", 13), 0x90c0abdc3ee773d1ULL},
    };
    for (const HashCase &test : hash_cases)
        expect(ring_hash(test.text) == test.expected, "mixed hash matches fixed vector");

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
        expect(ring[i].position == ring_hash(ring[i].group_id), "positions use mixed hash of stable IDs");
        expect(ring[i].position == reordered[i].position && ring[i].group_id == reordered[i].group_id,
               "configuration order does not change ring");
    }
    expect(ring[0].group_id == "group-2" && ring[1].group_id == "group-0" &&
           ring[2].group_id == "group-1", "fixed mixed positions have expected order");
    expect(group_for_key(ring, "color") == "group-1", "lookup uses mixed key hash");
    expect(group_for_key(ring, "") == "group-2", "mixed key past last entry wraps");
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

    const auto virtual_before = build_ring({"group-0", "group-1"}, 64);
    const auto virtual_after = build_ring({"group-0", "group-1", "group-2"}, 64);
    const auto virtual_reordered = build_ring({"group-2", "group-1", "group-0"}, 64);
    const auto virtual_removed = build_ring({"group-0", "group-2"}, 64);
    expect(virtual_before.size() == 128 && virtual_after.size() == 192,
           "each real group contributes the requested number of entries");
    for (const std::string &id : {"group-0", "group-1", "group-2"})
    {
        std::size_t count = 0;
        for (const RingEntry &entry : virtual_after)
            if (entry.group_id == id)
                ++count;
        expect(count == 64, "virtual entries point back to their real group");
    }
    for (std::size_t i = 0; i < virtual_after.size(); ++i)
        expect(virtual_after[i].position == virtual_reordered[i].position &&
               virtual_after[i].group_id == virtual_reordered[i].group_id,
               "virtual positions are independent of membership input order");
    // Raising the count preserves the existing entries, including the original
    // bare-ID position. It may still change ownership and would need migration.
    const auto three_positions = build_ring({"group-0"}, 3);
    const auto more_positions = build_ring({"group-0"}, 64);
    for (const auto position : {0x30eb621c7adc1a84ULL, 0x833b40f2c4518018ULL, 0x420c5409217f812aULL})
    {
        bool found_before = false, found_after = false;
        for (const RingEntry &entry : three_positions)
            found_before = found_before || entry.position == position;
        for (const RingEntry &entry : more_positions)
            found_after = found_after || entry.position == position;
        expect(found_before && found_after, "token positions have fixed hashes as count grows");
    }
    for (std::size_t i = 0; i < 10000; ++i)
    {
        const auto key = "experiment-key-" + std::to_string(i);
        const auto old_owner = group_for_key(virtual_before, key);
        const auto new_owner = group_for_key(virtual_after, key);
        expect(new_owner == old_owner || new_owner == "group-2",
               "adding virtual nodes for a new group never moves keys between old groups");
        if (new_owner != "group-1")
            expect(group_for_key(virtual_removed, key) == new_owner,
                   "removing a group only moves that group's keys");
        expect(group_for_key(more_positions, key) == "group-0",
               "many virtual positions for one group still return the real group");
    }
    const std::vector<RingEntry> tied{{20, "a"}, {20, "b"}, {70, "c"}};
    expect(group_for_position(tied, 20) == "a", "collision uses first ID in sorted order");
    expect(group_for_position(tied, 21) == "c", "colliding entries do not create a new interval");
    expect_invalid([] { build_ring({}); }, "reject empty membership");
    expect_invalid([] { build_ring({""}); }, "reject empty ID");
    expect_invalid([] { build_ring({"same", "same"}); }, "reject duplicate ID");
    expect_invalid([] { build_ring({"same", "same"}, 64); }, "reject duplicate ID with virtual nodes");
    expect_invalid([] { build_ring({"group-0"}, 0); }, "reject zero positions per group");
    expect_invalid([] { group_for_position({}, 33); }, "reject empty lookup");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "All hash ring tests passed.\n";
    return EXIT_SUCCESS;
}
