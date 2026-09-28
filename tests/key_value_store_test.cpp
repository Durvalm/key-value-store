#include "key_value_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

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

    std::filesystem::path test_file(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("kv_store_" + name + ".log");
    }

    void write_file(const std::filesystem::path &path, const std::string &contents)
    {
        std::ofstream file(path, std::ios::trunc);
        file << contents;
    }

    std::string read_file(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    void expect_runtime_error(
        const std::function<void()> &operation,
        const std::string &message)
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

    void test_new_store_is_empty()
    {
        KeyValueStore store;

        expect(store.size() == 0, "a new store has size zero");
        expect(!store.get("missing").has_value(), "a missing key returns nullopt");
    }

    void test_set_and_get()
    {
        KeyValueStore store;

        store.set("name", "Durval");

        const auto value = store.get("name");
        expect(value.has_value(), "get finds a stored key");
        expect(value.value_or("") == "Durval", "get returns the stored value");
        expect(store.size() == 1, "set increases the store size");
    }

    void test_set_overwrites_existing_value()
    {
        KeyValueStore store;
        store.set("language", "Python");
        store.set("language", "C++");

        expect(store.get("language").value_or("") == "C++", "set overwrites a value");
        expect(store.size() == 1, "overwriting does not add another key");
    }

    void test_remove_existing_key()
    {
        KeyValueStore store;
        store.set("temporary", "value");

        expect(store.remove("temporary"), "remove reports an existing key");
        expect(!store.get("temporary").has_value(), "removed keys cannot be retrieved");
        expect(store.size() == 0, "remove decreases the store size");
    }

    void test_remove_missing_key()
    {
        KeyValueStore store;

        expect(!store.remove("missing"), "remove reports a missing key");
        expect(store.size() == 0, "removing a missing key does not change the size");
    }

    void test_keys_are_independent()
    {
        KeyValueStore store;
        store.set("first", "one");
        store.set("second", "two");
        store.remove("first");

        expect(!store.get("first").has_value(), "the selected key is removed");
        expect(store.get("second").value_or("") == "two", "other keys remain stored");
        expect(store.size() == 1, "only one key remains");
    }

    void test_missing_persistence_file_is_created()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("creation_test");
        fs::remove(file_path);

        {
            KeyValueStore store(file_path.string());
            expect(fs::exists(file_path), "opening a missing persistence file creates it");
            expect(store.size() == 0, "a new persistence file produces an empty store");
        }

        fs::remove(file_path);
    }

    void test_mutations_survive_restart()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("restart_test");
        fs::remove(file_path);

        {
            KeyValueStore store(file_path.string());
            store.set("language", "Python");
            store.set("language", "C++");
            store.set("full name", "Durval \"D\" Almeida");
            store.set("temporary", "first");
            store.remove("temporary");
            store.set("temporary", "second");
        }

        {
            KeyValueStore recovered(file_path.string());
            expect(recovered.size() == 3, "replay reconstructs the number of live keys");
            expect(recovered.get("language").value_or("") == "C++", "replay keeps the newest value");
            expect(
                recovered.get("full name").value_or("") == "Durval \"D\" Almeida",
                "quoted keys and values survive replay");
            expect(
                recovered.get("temporary").value_or("") == "second",
                "a deleted key can be created again");
        }

        fs::remove(file_path);
    }

    void test_missing_delete_does_not_change_log()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("missing_delete_test");
        fs::remove(file_path);

        KeyValueStore store(file_path.string());
        const auto size_before = fs::file_size(file_path);
        expect(!store.remove("missing"), "removing a missing persistent key returns false");
        expect(
            fs::file_size(file_path) == size_before,
            "removing a missing key does not append a record");

        fs::remove(file_path);
    }

    void test_open_without_mutation_leaves_log_unchanged()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("unchanged_test");
        const std::string original = "SET \"name\" \"Durval\"\n";
        write_file(file_path, original);

        {
            KeyValueStore store(file_path.string());
            expect(store.get("name").value_or("") == "Durval", "an existing log is replayed");
        }

        expect(read_file(file_path) == original, "replay does not append records");
        fs::remove(file_path);
    }

    void test_invalid_complete_records_fail_recovery()
    {
        namespace fs = std::filesystem;
        const fs::path unknown_path = test_file("unknown_operation_test");
        write_file(unknown_path, "DESTROY \"name\"\n");
        expect_runtime_error(
            [&unknown_path]() { KeyValueStore store(unknown_path.string()); },
            "an unknown operation stops recovery");
        fs::remove(unknown_path);

        const fs::path malformed_path = test_file("malformed_record_test");
        write_file(malformed_path, "DELETE \"name\" \"unexpected\"\n");
        expect_runtime_error(
            [&malformed_path]() { KeyValueStore store(malformed_path.string()); },
            "extra DELETE fields stop recovery");
        fs::remove(malformed_path);
    }

    void test_incomplete_final_record_is_ignored()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("incomplete_tail_test");
        write_file(file_path, "SET \"complete\" \"yes\"\nSET \"partial\" \"no\"");

        KeyValueStore store(file_path.string());
        expect(store.get("complete").value_or("") == "yes", "complete records are replayed");
        expect(!store.contains("partial"), "an incomplete final record is ignored");

        fs::remove(file_path);
    }
} // namespace

void test_contains()
{
    KeyValueStore store;

    expect(!store.contains("name"), "contains reports a missing key");

    store.set("name", "Durval");
    expect(store.contains("name"), "contains reports an existing key");

    store.remove("name");
    expect(!store.contains("name"), "contains reports a removed key");
}

int main()
{
    test_new_store_is_empty();
    test_set_and_get();
    test_set_overwrites_existing_value();
    test_remove_existing_key();
    test_remove_missing_key();
    test_keys_are_independent();
    test_contains();
    test_missing_persistence_file_is_created();
    test_mutations_survive_restart();
    test_missing_delete_does_not_change_log();
    test_open_without_mutation_leaves_log_unchanged();
    test_invalid_complete_records_fail_recovery();
    test_incomplete_final_record_is_ignored();

    if (failures != 0)
    {
        std::cerr << failures << " test assertion(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "All key-value store tests passed.\n";
    return EXIT_SUCCESS;
}
