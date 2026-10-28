#include "cluster_layout.h"
#include "partition.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        catch (const std::runtime_error &)
        {
        }
    }
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "kv_layout_test";
    std::filesystem::create_directories(directory);
    const auto path = directory / "layout.txt";
    const std::vector<GroupDefinition> groups{
        {"group-0", "history-0", 6380, 6381, (directory / "primary-0.log").string(), (directory / "replica-0.log").string()},
        {"group-1", "history-1", 6382, 6383, (directory / "primary-1.log").string(), (directory / "replica-1.log").string()},
        {"group-2", "history-2", 6384, 6385, (directory / "primary-2.log").string(), (directory / "replica-2.log").string()},
    };
    const ClusterLayout modulo("fnv1a64-mod-v1", 1, {groups[0], groups[1]});
    const ClusterLayout ring("fnv1a64-fmix64-ring-v1", 64, groups);
    const ClusterLayout reordered("fnv1a64-fmix64-ring-v1", 64, {groups[2], groups[0], groups[1]});
    expect(ring.ownership_definition() == reordered.ownership_definition(), "ring identity canonicalizes member order");
    const ClusterLayout reversed_modulo("fnv1a64-mod-v1", 1, {groups[1], groups[0]});
    expect(modulo.ownership_definition() != reversed_modulo.ownership_definition(), "modulo identity preserves index order");
    const ClusterLayout changed_count("fnv1a64-fmix64-ring-v1", 32, groups);
    expect(ring.ownership_definition() != changed_count.ownership_definition(), "virtual count changes ownership identity");
    const auto expected_ring = build_ring({"group-0", "group-1", "group-2"}, 64);
    for (std::size_t i = 0; i < 1000; ++i)
    {
        const std::string key = "layout-key-" + std::to_string(i);
        expect(modulo.owner(key).id == groups[partition_for_key(key)].id, "two-group modulo agrees with live routing");
        expect(ring.owner(key).id == group_for_key(expected_ring, key), "layout uses existing ring rule");
        expect(ring.owner(key).id == reordered.owner(key).id, "ring ignores configuration ordering");
    }
    expect(modulo.owner("color").primary_port == 6380, "owner includes primary endpoint");
    expect(ring.group("group-2").history_id == "history-2", "group definition includes history identity");
    expect_invalid([&] { ring.group("missing"); }, "reject unknown group");
    expect_invalid([&] { ClusterLayout bad("unknown", 1, groups); }, "reject unsupported routing");
    expect_invalid([&] { ClusterLayout bad("fnv1a64-mod-v1", 64, groups); }, "modulo has no virtual count");
    expect_invalid([&] { ClusterLayout bad("fnv1a64-fmix64-ring-v1", 0, groups); }, "reject zero positions");
    expect_invalid([&] { ClusterLayout bad("fnv1a64-fmix64-ring-v1", 1025, groups); }, "reject excessive positions");
    expect_invalid([&] { ClusterLayout bad("fnv1a64-mod-v1", 1, {}); }, "reject empty layout");
    for (int mutation = 0; mutation < 5; ++mutation)
    {
        auto invalid = groups;
        if (mutation == 0) invalid[1].id = invalid[0].id;
        if (mutation == 1) invalid[1].history_id = invalid[0].history_id;
        if (mutation == 2) invalid[1].replica_port = invalid[0].primary_port;
        if (mutation == 3) invalid[1].primary_log = (directory / "unused/../primary-0.log").string();
        if (mutation == 4) invalid[1].primary_port = 0;
        expect_invalid([&] { ClusterLayout bad("fnv1a64-mod-v1", 1, invalid); }, "reject conflicting node definitions");
    }

    const std::string header = "LAYOUT 1 fnv1a64-mod-v1 1\n";
    const std::string group = "GROUP \"group-0\" \"history-0\" 6380 6381 \"logs with spaces/primary.log\" \"replica.log\"\n";
    auto write = [&](const std::string &text)
    {
        std::ofstream file(path);
        file << text;
    };
    write(header + group);
    const auto parsed = read_layout(path.string());
    expect(parsed.owner("any-key").primary_log == (std::filesystem::weakly_canonical(directory) / "logs with spaces/primary.log").string(),
           "relative paths resolve against layout directory, preserving spaces");
    expect(!std::filesystem::exists(directory / "logs with spaces"), "reading layout does not create log directories");
    for (const auto &text : {std::string(""), header,
                            std::string("LAYOUT 2 fnv1a64-mod-v1 1\n") + group,
                            header + "GROUP unquoted \"history\" 6380 6381 \"a\" \"b\"\n",
                            header + "GROUP \"g\" \"h\" 65536 6381 \"a\" \"b\"\n",
                            header + "GROUP \"g\" \"h\" 6380 6381 \"\" \"b\"\n",
                            header + group + "unexpected\n"})
    {
        write(text);
        expect_invalid([&] { read_layout(path.string()); }, "reject malformed layout file");
    }
    expect_invalid([&] { read_layout((directory / "missing.layout").string()); }, "reject missing layout");
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "All cluster layout tests passed.\n";
}
