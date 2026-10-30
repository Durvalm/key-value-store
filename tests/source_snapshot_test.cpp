#include "source_snapshot.h"
#include "key_value_store.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    void expect_error(Operation operation, const std::string &message)
    {
        try { operation(); expect(false, message); }
        catch (const std::runtime_error &) {}
    }
    std::string read(const std::string &path)
    {
        std::ifstream file(path);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }
    void write(const std::string &path, const std::string &contents)
    {
        std::ofstream file(path);
        file << contents;
    }
    std::string owned_key(const ClusterLayout &layout, const std::string &group, const std::string &prefix)
    {
        for (int i = 0; i < 10000; ++i)
        {
            const std::string key = prefix + std::to_string(i);
            if (layout.owner(key).id == group)
                return key;
        }
        throw std::runtime_error("Could not find test key");
    }
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("kv_snapshot_test_" + std::to_string(::getpid()));
    if (!std::filesystem::create_directory(directory))
        return EXIT_FAILURE;
    try
    {
        std::vector<GroupDefinition> groups;
        for (int i = 0; i < 2; ++i)
            groups.push_back({"group-" + std::to_string(i), "history-" + std::to_string(i),
                static_cast<std::uint16_t>(6380 + 2*i), static_cast<std::uint16_t>(6381 + 2*i),
                (directory / ("primary-" + std::to_string(i) + ".log")).string(),
                (directory / ("replica-" + std::to_string(i) + ".log")).string()});
        const ClusterLayout layout("fnv1a64-fmix64-ring-v1", 64, groups);
        std::vector<std::pair<std::string, std::string>> expected;
        for (const auto &group : groups)
        {
            const auto key = owned_key(layout, group.id, "quoted \\\" cafe \xc3\xa9 ");
            const auto deleted = owned_key(layout, group.id, "deleted-");
            const auto recreated = owned_key(layout, group.id, "recreated-");
            std::vector<std::string> history;
            {
                KeyValueStore primary(group.primary_log, layout, group.id);
                primary.set(key, "old-value");
                primary.set(key, "latest-value");
                primary.set(deleted, "removed-value");
                primary.remove(deleted);
                primary.set(recreated, "old-value");
                primary.remove(recreated);
                primary.set(recreated, "recreated-value");
                history = primary.history_records();
            }
            {
                KeyValueStore replica(group.replica_log, layout, group.id);
                for (const auto &record : history)
                    replica.apply_replica_record(record);
            }
            expected.push_back({key, "latest-value"});
            expected.push_back({recreated, "recreated-value"});
        }
        std::sort(expected.begin(), expected.end());
        std::vector<std::string> original;
        for (const auto &group : groups)
        {
            original.push_back(read(group.primary_log));
            original.push_back(read(group.replica_log));
        }
        const auto snapshot = collect_source_snapshot(layout);
        expect(snapshot.entries == expected, "snapshot contains only latest live primary values, without replica duplicates");
        expect(snapshot.sources.size() == 2 && snapshot.sources[0].last_sequence == 7 &&
               snapshot.sources[1].last_sequence == 7 && snapshot.sources[0].live_keys == 2 &&
               snapshot.sources[1].live_keys == 2, "snapshot records validated source progress and counts");
        for (std::size_t i = 0; i < groups.size(); ++i)
            expect(read(groups[i].primary_log) == original[2*i] && read(groups[i].replica_log) == original[2*i+1],
                   "collection preserves every source log byte");
        const auto output = (directory / "snapshot.txt").string();
        write_source_snapshot(snapshot, output);
        const auto contents = read(output);
        expect(contents.starts_with("SNAPSHOT 1 " + layout.ownership_definition()) && contents.ends_with("END 4\n"),
               "snapshot includes routing definition and completion count");
        expect(contents.find("old-value") == std::string::npos && contents.find("removed-value") == std::string::npos,
               "export excludes obsolete and deleted values");
        expect_error([&] { write_source_snapshot(snapshot, output); }, "existing snapshots are not overwritten");
        expect_error([&] { write_source_snapshot(snapshot, groups[0].primary_log); }, "source cannot become output");
        expect(read(output) == contents && read(groups[0].primary_log) == original[0], "output refusal preserves files");
        {
            KeyValueStore source(groups[0].primary_log, layout, groups[0].id, true);
            expect_error([&] { source.set(expected[0].first, "wrong"); }, "read-only replay cannot write");
            expect_error([&] { source.remove("missing"); }, "read-only replay rejects even missing deletes");
            expect_error([&] { source.apply_replica_record("1 SET \"wrong\" \"wrong\""); }, "read-only peer application is disabled");
            expect_error([&] { collect_source_snapshot(layout); }, "busy source prevents snapshot");
        }
        expect(collect_source_snapshot(layout).entries == expected, "failed collection releases earlier locks");
        // Same record count and final state are insufficient: compare complete histories.
        auto conflict = original[1];
        conflict.replace(conflict.find("old-value"), 9, "different");
        write(groups[0].replica_log, conflict);
        expect_error([&] { collect_source_snapshot(layout); }, "conflicting history is rejected even with identical final values");
        expect(read(groups[0].replica_log) == conflict, "rejected replica history is not repaired or modified");
        write(groups[0].replica_log, original[1]);
        {
            KeyValueStore primary(groups[0].primary_log, layout, groups[0].id);
            primary.set(owned_key(layout, groups[0].id, "extra-"), "unconfirmed");
        }
        expect_error([&] { collect_source_snapshot(layout); }, "behind replica must catch up first");
        write(groups[0].primary_log, original[0]);
        write(groups[0].primary_log, "");
        expect_error([&] { collect_source_snapshot(layout); }, "empty source cannot be initialized as fresh data");
        expect(read(groups[0].primary_log).empty(), "empty source is not given new headers");
        write(groups[0].primary_log, original[0]);
        std::filesystem::remove(groups[0].replica_log);
        expect_error([&] { collect_source_snapshot(layout); }, "missing source is rejected");
        expect(!std::filesystem::exists(groups[0].replica_log), "missing source is not recreated");
        std::filesystem::create_hard_link(groups[0].primary_log, groups[0].replica_log);
        expect_error([&] { collect_source_snapshot(layout); }, "hard-linked copies are not independent sources");

        const GroupDefinition empty_group{"empty", "empty-history", 6390, 6391,
            (directory / "empty-primary.log").string(), (directory / "empty-replica.log").string()};
        const ClusterLayout empty_layout("fnv1a64-mod-v1", 1, {empty_group});
        std::vector<std::string> empty_history;
        {
            KeyValueStore primary(empty_group.primary_log, empty_layout, "empty");
            primary.set("deleted", "value");
            primary.remove("deleted");
            empty_history = primary.history_records();
        }
        {
            KeyValueStore replica(empty_group.replica_log, empty_layout, "empty");
            for (const auto &record : empty_history)
                replica.apply_replica_record(record);
        }
        const auto empty = collect_source_snapshot(empty_layout);
        expect(empty.entries.empty() && empty.sources[0].last_sequence == 2 && empty.sources[0].live_keys == 0,
               "a valid group with all keys deleted exports zero live values");

        const auto compact_path = (directory / "compact.log").string();
        {
            KeyValueStore writer(compact_path);
            writer.set("key", "value");
            writer.compact();
            expect_error([&] { KeyValueStore second(compact_path); }, "file lock survives compaction replacing log inode");
        }
        {
            KeyValueStore reopened(compact_path);
            expect(reopened.get("key") == "value", "normal reopen works after owner releases lock");
        }
        KeyValueStore memory;
        memory.set("key", "before");
        auto detached = memory.snapshot();
        memory.set("key", "after");
        expect(detached[0].second == "before", "snapshot is detached from subsequent updates");
    }
    catch (const std::exception &error)
    {
        std::cerr << "Unexpected test failure: " << error.what() << '\n';
        ++failures;
    }
    std::filesystem::remove_all(directory);
    if (failures)
        return EXIT_FAILURE;
    std::cout << "All source snapshot tests passed.\n";
}
