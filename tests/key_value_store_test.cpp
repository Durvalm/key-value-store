#include "command_processor.h"
#include "key_value_store.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <sstream>
#include <string>
#include <thread>

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

    std::size_t count_records(const std::string &contents)
    {
        std::size_t record_count = 0;

        for (char character : contents)
        {
            if (character == '\n')
            {
                ++record_count;
            }
        }

        return record_count;
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

    void run_concurrently(
        const std::function<void()> &first_operation,
        const std::function<void()> &second_operation)
    {
        std::latch start(2);
        std::exception_ptr first_error;
        std::exception_ptr second_error;

        auto run = [&start](const std::function<void()> &operation,
                            std::exception_ptr &error)
        {
            start.arrive_and_wait();
            try
            {
                operation();
            }
            catch (...)
            {
                error = std::current_exception();
            }
        };

        std::thread first(run, std::cref(first_operation), std::ref(first_error));
        std::thread second(run, std::cref(second_operation), std::ref(second_error));
        first.join();
        second.join();

        // Keep the assertion counter and diagnostics on the main thread.
        expect(!first_error, "first concurrent worker completes without throwing");
        expect(!second_error, "second concurrent worker completes without throwing");
    }

    void test_concurrent_distinct_keys()
    {
        KeyValueStore store;
        auto insert = [&store](const std::string &prefix)
        {
            for (int i = 0; i < 1000; ++i)
            {
                store.set(prefix + std::to_string(i), std::to_string(i));
            }
        };

        run_concurrently([&]() { insert("a-"); }, [&]() { insert("b-"); });

        expect(store.size() == 2000, "concurrent inserts preserve all 2000 keys");
        for (const std::string prefix : {"a-", "b-"})
        {
            for (int i = 0; i < 1000; ++i)
            {
                const std::string key = prefix + std::to_string(i);
                expect(store.get(key).value_or("") == std::to_string(i),
                       "concurrent insert preserves value for " + key);
            }
        }
    }

    void test_concurrent_delete()
    {
        KeyValueStore store;
        store.set("shared", "value");
        bool first_removed = false;
        bool second_removed = false;

        run_concurrently(
            [&]() { first_removed = store.remove("shared"); },
            [&]() { second_removed = store.remove("shared"); });

        expect(first_removed != second_removed, "exactly one concurrent DELETE succeeds");
        expect(!store.contains("shared"), "concurrently deleted key is absent");
        expect(store.size() == 0, "concurrent deletes leave an empty store");
    }

    void test_concurrent_persistence()
    {
        const auto file_path = test_file("concurrent_restart_test");
        std::filesystem::remove(file_path);
        std::string final_shared_value;

        {
            KeyValueStore store(file_path.string());
            auto insert = [&store](const std::string &prefix)
            {
                for (int i = 0; i < 100; ++i)
                {
                    const std::string key = prefix + std::to_string(i);
                    store.set(key, key);
                    store.set("shared", key);
                }
            };

            run_concurrently([&]() { insert("a-"); }, [&]() { insert("b-"); });
            expect(store.size() == 201, "concurrent persistent writes preserve all keys");
            final_shared_value = store.get("shared").value_or("");
            expect(final_shared_value == "a-99" || final_shared_value == "b-99",
                   "either concurrent writer may produce the final shared value");
        }

        {
            KeyValueStore recovered(file_path.string());
            expect(recovered.size() == 201, "concurrent log replays the correct key count");
            expect(recovered.get("shared").value_or("") == final_shared_value,
                   "log ordering agrees with the final in-memory shared value");
            for (const std::string prefix : {"a-", "b-"})
            {
                for (int i = 0; i < 100; ++i)
                {
                    const std::string key = prefix + std::to_string(i);
                    expect(recovered.get(key).value_or("") == key,
                           "concurrent persistent value survives restart: " + key);
                }
            }
        }

        std::filesystem::remove(file_path);
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

    void test_compaction_preserves_state_and_removes_history()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("compaction_test");
        fs::path temporary_path = file_path;
        temporary_path += ".tmp";
        fs::remove(file_path);
        fs::remove(temporary_path);

        {
            KeyValueStore store(file_path.string());
            store.set("language", "Python");
            store.set("language", "Java");
            store.set("language", "C++");
            store.set("temporary", "value");
            store.remove("temporary");
            store.set("full name", "Durval Almeida");

            expect(
                count_records(read_file(file_path)) == 6,
                "the append-only log contains historical mutations before compaction");

            store.compact();

            const std::string compacted_log = read_file(file_path);
            expect(
                count_records(compacted_log) == 2,
                "compaction writes one SET record per live key");
            expect(
                compacted_log.find("DELETE") == std::string::npos,
                "a compacted log contains no DELETE records");
            expect(
                !fs::exists(temporary_path),
                "the temporary file is renamed away after successful compaction");
        }

        {
            KeyValueStore recovered(file_path.string());
            expect(recovered.size() == 2, "compacted state survives a restart");
            expect(
                recovered.get("language").value_or("") == "C++",
                "compaction keeps the newest value");
            expect(
                recovered.get("full name").value_or("") == "Durval Almeida",
                "compaction keeps independent live keys");
            expect(!recovered.contains("temporary"), "compaction keeps deleted keys absent");

            recovered.compact();
            expect(
                count_records(read_file(file_path)) == 2,
                "compacting an already compacted log preserves its record count");
        }

        fs::remove(file_path);
        fs::remove(temporary_path);
    }

    void test_in_memory_compaction_is_a_no_op()
    {
        KeyValueStore store;
        store.set("name", "Durval");

        store.compact();

        expect(
            store.get("name").value_or("") == "Durval",
            "in-memory compaction leaves the store unchanged");
    }

    void test_mutations_append_after_compaction()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("post_compaction_append_test");
        fs::remove(file_path);

        {
            KeyValueStore store(file_path.string());
            store.set("language", "Python");
            store.set("language", "C++");
            store.compact();
            store.set("project", "key-value store");
        }

        {
            KeyValueStore recovered(file_path.string());
            expect(recovered.size() == 2, "post-compaction append survives restart");
            expect(
                recovered.get("language").value_or("") == "C++",
                "compacted data remains after a later append");
            expect(
                recovered.get("project").value_or("") == "key-value store",
                "a later append is written to the compacted log");
        }

        fs::remove(file_path);
    }

    void test_command_processor_mutations_and_queries()
    {
        KeyValueStore store;

        const CommandResult set_result = process_command(
            "SET \"full name\" \"Durval \\\"D\\\" Almeida\"",
            store);
        expect(set_result.response == "OK\n", "SET returns the protocol success response");
        expect(!set_result.close_requested, "SET keeps the connection open");

        const CommandResult get_result = process_command("GET \"full name\"", store);
        expect(
            get_result.response == "VALUE \"Durval \\\"D\\\" Almeida\"\n",
            "GET returns an escaped typed value response");

        expect(
            process_command("EXISTS \"full name\"", store).response == "INTEGER 1\n",
            "EXISTS returns a typed integer response");
        expect(
            process_command("SIZE", store).response == "INTEGER 1\n",
            "SIZE returns a typed integer response");
        expect(
            process_command("DELETE \"full name\"", store).response == "INTEGER 1\n",
            "DELETE reports a removed key");
        expect(
            process_command("GET \"full name\"", store).response == "NOT_FOUND\n",
            "GET distinguishes a missing key from a stored value");
    }

    void test_command_processor_rejects_malformed_requests()
    {
        KeyValueStore store;

        expect(
            process_command("", store).response.rfind("ERROR EMPTY_REQUEST", 0) == 0,
            "an empty request returns a protocol error");
        expect(
            process_command("SET name value", store).response.rfind("ERROR INVALID_ARGUMENT", 0) == 0,
            "unquoted SET fields are rejected");
        expect(
            process_command("GET \"name\" extra", store).response.rfind("ERROR INVALID_ARGUMENT", 0) == 0,
            "extra command arguments are rejected");
        expect(
            process_command("UNKNOWN", store).response.rfind("ERROR UNKNOWN_COMMAND", 0) == 0,
            "an unknown command returns a typed error");
        expect(store.size() == 0, "malformed requests do not mutate the store");

        const std::string oversized_request(4097, 'x');
        expect(
            process_command(oversized_request, store).response.rfind("ERROR REQUEST_TOO_LARGE", 0) == 0,
            "requests over the protocol limit are rejected");
    }

    void test_command_processor_connection_result()
    {
        KeyValueStore store;

        const CommandResult exit_result = process_command("EXIT", store);
        expect(exit_result.response == "BYE\n", "EXIT returns the protocol goodbye response");
        expect(exit_result.close_requested, "EXIT requests that the caller close its connection");

        const CommandResult help_result = process_command("HELP", store);
        expect(
            help_result.response.rfind("COMMANDS ", 0) == 0,
            "HELP returns one framed protocol response");
        expect(!help_result.close_requested, "HELP keeps the connection open");
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
    test_concurrent_distinct_keys();
    test_concurrent_delete();
    test_concurrent_persistence();
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
    test_compaction_preserves_state_and_removes_history();
    test_in_memory_compaction_is_a_no_op();
    test_mutations_append_after_compaction();
    test_command_processor_mutations_and_queries();
    test_command_processor_rejects_malformed_requests();
    test_command_processor_connection_result();

    if (failures != 0)
    {
        std::cerr << failures << " test assertion(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "All key-value store tests passed.\n";
    return EXIT_SUCCESS;
}
