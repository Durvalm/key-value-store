#include "hash_ring.h"
#include "partition.h"

#include <array>
#include <iomanip>
#include <iostream>
#include <string>

int main()
{
    // Small, manually chosen positions demonstrate lookup, not actual hashes.
    const std::vector<RingEntry> toy_before{{20, "group-0"}, {70, "group-1"}};
    const std::vector<RingEntry> toy_after{{20, "group-0"}, {45, "group-2"}, {70, "group-1"}};
    std::cout << "Toy lookup (illustrative positions):\n";
    for (std::uint64_t position : {12, 33, 60, 85})
        std::cout << "  key position " << position << ": "
                  << group_for_position(toy_before, position) << " -> "
                  << group_for_position(toy_after, position) << '\n';
    std::size_t toy_moved = 0;
    for (std::uint64_t position = 0; position < 100; ++position)
        if (group_for_position(toy_before, position) != group_for_position(toy_after, position))
            ++toy_moved;
    std::cout << "Toy ring: " << toy_moved << "/100 positions change owners (21 through 45).\n\n";

    const std::vector<std::string> ids{"group-0", "group-1", "group-2"};
    const auto before = build_ring({ids[0], ids[1]});
    const auto after = build_ring(ids);
    std::cout << "Mixed FNV-1a + fmix64 group positions (64-bit, decimal):\n";
    for (const RingEntry &entry : after)
        std::cout << "  " << entry.group_id << ": " << entry.position << '\n';

    constexpr std::size_t key_count = 10000;
    std::array<std::size_t, 3> modulo_before{}, modulo_after{}, ring_before{}, ring_after{};
    std::size_t modulo_moved = 0, ring_moved = 0;
    for (std::size_t i = 0; i < key_count; ++i)
    {
        const std::string key = "experiment-key-" + std::to_string(i);
        const auto hash = stable_key_hash(key);
        const auto old_modulo = hash % 2;
        const auto new_modulo = hash % 3;
        ++modulo_before[old_modulo];
        ++modulo_after[new_modulo];
        if (old_modulo != new_modulo)
            ++modulo_moved;
        const auto old_ring = group_for_key(before, key);
        const auto new_ring = group_for_key(after, key);
        for (std::size_t group = 0; group < ids.size(); ++group)
        {
            if (old_ring == ids[group])
                ++ring_before[group];
            if (new_ring == ids[group])
                ++ring_after[group];
        }
        if (old_ring != new_ring)
            ++ring_moved;
    }
    std::cout << "\nDataset: experiment-key-0 through experiment-key-9999\n"
              << "Strategy       group-0  group-1  group-2\n";
    auto print_counts = [](const std::string &label, const std::array<std::size_t, 3> &counts)
    {
        std::cout << std::left << std::setw(15) << label << std::right;
        for (auto count : counts)
            std::cout << std::setw(9) << count;
        std::cout << '\n';
    };
    print_counts("Modulo, 2", modulo_before);
    print_counts("Modulo, 3", modulo_after);
    print_counts("Ring, 2", ring_before);
    print_counts("Ring, 3", ring_after);
    std::cout << std::fixed << std::setprecision(2)
              << "Modulo moved: " << modulo_moved << "/" << key_count
              << " (" << 100.0 * modulo_moved / key_count << "%)\n"
              << "Ring moved: " << ring_moved << "/" << key_count
              << " (" << 100.0 * ring_moved / key_count << "%)\n\n"
              << "One position per group can still be imbalanced; mixing is not a balance guarantee.\n"
              << "This is an ownership experiment, not live routing or data migration.\n";
}
