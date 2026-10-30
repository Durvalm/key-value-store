#include "destination_transfer.h"
#include "key_value_store.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    int failures = 0;
    void expect(bool condition, const std::string &message)
    {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    }
    template<typename Operation>
    void expect_error(Operation operation, const std::string &message)
    {
        try { operation(); expect(false, message); }
        catch (const std::runtime_error &) {}
    }
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("kv_transfer_test_" + std::to_string(::getpid()));
    if (!std::filesystem::create_directory(directory))
        return EXIT_FAILURE;
    try
    {
        GroupDefinition old{"group-0", "old-history", 6380, 6381,
            (directory / "old-primary.log").string(), (directory / "old-replica.log").string()};
        GroupDefinition fresh{"group-0", "new-history", 6480, 6481,
            (directory / "new-primary.log").string(), (directory / "new-replica.log").string()};
        const ClusterLayout source("fnv1a64-mod-v1", 1, {old});
        const ClusterLayout destination("fnv1a64-fmix64-ring-v1", 64, {fresh});
        validate_transfer_layouts(source, destination);
        {
            KeyValueStore primary(old.primary_log, source, old.id);
            KeyValueStore replica(old.replica_log, source, old.id);
        }
        {
            OfflineSources sources(source);
            expect(sources.snapshot().entries.empty(), "empty source is a valid snapshot");
            expect_error([&] { KeyValueStore writer(old.primary_log, source, old.id); },
                         "offline guard retains locks after snapshot returns");
        }
        {
            KeyValueStore writer(old.primary_log, source, old.id);
            expect(writer.last_sequence() == 0, "guard destruction releases source lock");
        }
        auto invalid = fresh;
        invalid.history_id = old.history_id;
        expect_error([&] { validate_transfer_layouts(source, ClusterLayout("fnv1a64-mod-v1", 1, {invalid})); },
                     "destination cannot reuse source history");
        invalid = fresh;
        invalid.primary_log = old.primary_log;
        expect_error([&] { validate_transfer_layouts(source, ClusterLayout("fnv1a64-mod-v1", 1, {invalid})); },
                     "source and destination paths cannot overlap");
        invalid.primary_log = old.primary_log + ".lock";
        expect_error([&] { validate_transfer_layouts(source, ClusterLayout("fnv1a64-mod-v1", 1, {invalid})); },
                     "destination cannot replace a source lock file");
        const auto alias = (directory / "hardlink.log").string();
        std::filesystem::create_hard_link(old.primary_log, alias);
        invalid.primary_log = alias;
        expect_error([&] { validate_transfer_layouts(source, ClusterLayout("fnv1a64-mod-v1", 1, {invalid})); },
                     "hard links do not bypass overlap validation");
        {
            KeyValueStore primary(fresh.primary_log, destination, fresh.id);
            KeyValueStore replica(fresh.replica_log, destination, fresh.id);
            primary.set("already", "written");
        }
        SourceSnapshot empty;
        expect_error([&] { transfer_snapshot(empty, destination); }, "nonfresh log is rejected before networking");
        SourceSnapshot oversized;
        oversized.entries.push_back({"key", std::string(4096, 'x')});
        expect_error([&] { transfer_snapshot(oversized, destination); }, "oversized SET is rejected before any destination writes");
    }
    catch (const std::exception &error)
    {
        std::cerr << "Unexpected transfer test failure: " << error.what() << '\n';
        ++failures;
    }
    std::filesystem::remove_all(directory);
    if (failures) return EXIT_FAILURE;
    std::cout << "All destination transfer validation tests passed.\n";
}
