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
            store.set("language", "Rust");
            store.set("temporary", "remove after compaction");
            store.remove("temporary");
        }

        {
            KeyValueStore recovered(file_path.string());
            expect(recovered.size() == 2, "post-compaction append survives restart");
            expect(
                recovered.get("language").value_or("") == "Rust",
                "an overwrite after compaction reaches the replacement log");
            expect(!recovered.contains("temporary"), "a deletion after compaction survives restart");
            expect(
                recovered.get("project").value_or("") == "key-value store",
                "a later append is written to the compacted log");
        }

        fs::remove(file_path);
    }

    void test_failed_compaction_leaves_log_usable()
    {
        namespace fs = std::filesystem;
        const fs::path file_path = test_file("failed_compaction_open_stream_test");
        fs::path temporary_path = file_path;
        temporary_path += ".tmp";
        fs::remove(file_path);
        // A directory at the temporary file path prevents opening it for writing.
        fs::create_directory(temporary_path);
        {
            KeyValueStore store(file_path.string());
            store.set("before", "one");
            const std::string original = read_file(file_path);
            expect_runtime_error([&]() { store.compact(); }, "failed compaction reports an error");
            expect(read_file(file_path) == original, "failed compaction preserves the original log");
            store.set("after", "two");
        }
        {
            KeyValueStore recovered(file_path.string());
            expect(recovered.get("before").value_or("") == "one", "original data survives failed compaction");
            expect(recovered.get("after").value_or("") == "two", "writes after failed compaction remain recoverable");
        }
        fs::remove(temporary_path);
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
    void test_history_header()
    {
        const auto path = test_file("history_header");
        for (const bool existing_empty_file : {false, true})
        {
            std::filesystem::remove(path);
            if (existing_empty_file)
                write_file(path, "");
            {
                KeyValueStore store(path.string(), true, "experiment-a");
                expect(store.history_id() == "experiment-a", "new numbered history retains explicit ID");
                expect(store.last_sequence() == 0 && store.size() == 0,
                       "header is not a mutation");
                expect(read_file(path) == "HISTORY experiment-a\n", "new numbered log has one header");
            }
            {
                KeyValueStore store(path.string(), true);
                expect(store.history_id() == "experiment-a" && store.last_sequence() == 0,
                       "header-only history reopens without supplying an ID");
                expect(read_file(path) == "HISTORY experiment-a\n", "reopen does not duplicate header");
                store.set("color", "blue");
            }
            {
                KeyValueStore store(path.string(), true);
                expect(store.history_id() == "experiment-a" && store.last_sequence() == 1 &&
                       store.get("color") == "blue", "restart recovers ID, data and progress");
            }
            const auto saved = read_file(path);
            expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "experiment-b"); },
                                 "supplied ID cannot replace an existing history ID");
            expect(read_file(path) == saved, "ID mismatch leaves log unchanged");
        }

        for (const std::string header : {
                 "HISTORY\n", "HISTORY id extra\n", "HISTORY bad/id\n", "HISTORY id",
                 "OTHER id\n", "\n", "1 SET \"k\" \"v\"\n",
                 "HISTORY id\nHISTORY id\n"})
        {
            write_file(path, header);
            expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "experiment-a"); },
                                 "missing, malformed, incomplete or duplicate header is rejected");
            expect(read_file(path) == header, "failed header recovery does not rewrite the file");
        }
        write_file(path, "HISTORY " + std::string(129, 'a') + "\n");
        expect_runtime_error([&]() { KeyValueStore store(path.string(), true); },
                             "oversized saved ID is rejected");
        write_file(path, "");
        expect_runtime_error([&]() { KeyValueStore store(path.string(), true); },
                             "new history requires an explicit ID");
        expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "bad id"); },
                             "new history ID must be a single safe token");
        expect(read_file(path).empty(), "invalid or missing ID does not write a header");
        std::filesystem::remove(path);
        KeyValueStore standalone;
        expect(standalone.history_id().empty(), "standalone has no replication history ID");
    }

    void test_numbered_history()
    {
        const auto path = test_file("numbered");
        std::filesystem::remove(path);
        {
            KeyValueStore store(path.string(), true, "test-history");
            expect(store.last_sequence() == 0, "fresh numbered history starts at zero");
            store.set("color", "blue");
            store.set("color", "red");
            expect(!store.remove("missing"), "missing delete is not numbered");
            expect(store.last_sequence() == 2, "only actual mutations advance progress");
            expect(read_file(path) == "HISTORY test-history\n1 SET \"color\" \"blue\"\n2 SET \"color\" \"red\"\n",
                   "numbers are persisted with their mutations");
            expect_runtime_error([&]() { store.compact(); }, "numbered history cannot be compacted");
        }
        {
            KeyValueStore store(path.string(), true, "test-history");
            expect(store.last_sequence() == 2, "restart recovers progress");
            expect(store.get("color") == "red", "restart recovers data");
            auto records = store.history_records();
            expect(records.size() == 2 && records[0] == "1 SET \"color\" \"blue\"",
                   "history snapshot contains ordered replayed records without the header");
            records[0] = "changed copy";
            expect(store.history_records()[0] == "1 SET \"color\" \"blue\"",
                   "changing a history copy does not modify the store");
            expect(store.apply_replica_record("1 SET \"color\" \"blue\"") == 1,
                   "old matching resend is acknowledged");
            expect(store.get("color") == "red", "old resend cannot undo newer state");
            expect(store.last_sequence() == 2 && count_records(read_file(path)) == 3,
                   "resend does not change progress or append again");
            expect_runtime_error([&]() { store.apply_replica_record("2 SET \"color\" \"wrong\""); },
                                 "conflicting resend is rejected");
            expect_runtime_error([&]() { store.apply_replica_record("4 SET \"color\" \"gap\""); },
                                 "gap is rejected");
            expect_runtime_error([&]() { store.apply_replica_record("3 DELETE \"absent\""); },
                                 "replicated missing delete indicates inconsistent state");
            store.remove("color");
            expect(store.last_sequence() == 3, "new writes continue numbering after restart");
            expect(read_file(path).ends_with("3 DELETE \"color\"\n"), "delete persists its sequence");
        }
        {
            KeyValueStore store(path.string(), true, "test-history");
            expect(store.last_sequence() == 3 && !store.contains("color"),
                   "delete and progress survive restart");
            expect(store.apply_replica_record("4 SET \"full name\" \"Durval \\\"D\\\"\"") == 4,
                   "numbered parser handles quoted fields");
        }
        expect_runtime_error([&]() { KeyValueStore store(path.string()); },
                             "standalone rejects numbered history rather than mixing formats");
        std::filesystem::remove(path);
    }

    void test_invalid_numbered_history()
    {
        const auto path = test_file("invalid_numbered");
        for (const std::string record : {
                 "SET \"k\" \"legacy\"\n", "0 SET \"k\" \"v\"\n",
                 "-1 SET \"k\" \"v\"\n", "+1 SET \"k\" \"v\"\n",
                 "18446744073709551616 SET \"k\" \"v\"\n",
                 "2 SET \"k\" \"gap\"\n", "1 GET \"k\"\n", "\n",
                 "1 SET \"k\" \"v\"\n1 SET \"k\" \"v\"\n",
                 "1 SET \"k\" \"v\"\n2 SET \"k\" \"partial\""})
        {
            write_file(path, "HISTORY test-history\n" + record);
            expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "test-history"); },
                                 "invalid/incompatible/incomplete numbered history is rejected");
            expect(read_file(path) == "HISTORY test-history\n" + record, "failed recovery leaves original bytes unchanged");
        }
        std::filesystem::remove(path);
    }

    void test_concurrent_numbered_history()
    {
        const auto path = test_file("concurrent_numbered");
        std::filesystem::remove(path);
        {
            KeyValueStore store(path.string(), true, "test-history");
            auto write = [&]()
            {
                for (int i = 0; i < 100; ++i)
                    store.set("shared", std::to_string(i));
            };
            run_concurrently(write, write);
            expect(store.last_sequence() == 200, "concurrent mutations have consecutive numbers");
        }
        {
            KeyValueStore store(path.string(), true, "test-history");
            expect(store.last_sequence() == 200, "concurrent numbered log replays without gaps");
        }
        std::filesystem::remove(path);
    }
    void test_partition_ownership()
    {
        const auto path = test_file("partition_ownership");
        std::filesystem::remove(path);
        const std::string header = "PARTITION 0 fnv1a64-mod2-v1\n";
        {
            KeyValueStore store(path.string(), false, "", 0);
            expect(read_file(path) == header, "new partition log records its routing identity");
            expect(store.owns_key("color") && !store.owns_key("full name"), "store uses the shared ownership rule");
            expect(process_command("SET \"color\" \"blue\"", store).response == "OK\n", "owner accepts write");
            const auto before = read_file(path);
            for (const auto &command : {"SET \"full name\" \"Durval\"", "GET \"full name\"",
                                       "DELETE \"full name\"", "EXISTS \"full name\""})
            {
                const auto result = process_command(command, store);
                expect(result.response.starts_with("ERROR WRONG_PARTITION") && !result.mutation_applied,
                       "every wrong-owner command is rejected");
            }
            expect(process_command("SET \"full name\"", store).response.starts_with("ERROR INVALID_ARGUMENT"),
                   "syntax is checked before ownership");
            expect_runtime_error([&]() { store.set("full name", "Durval"); }, "direct wrong-key mutation rejected");
            expect(store.size() == 1 && read_file(path) == before, "rejection changes neither map nor log");
            store.compact();
            expect(read_file(path).starts_with(header), "compaction retains partition header");
            store.set("color", "red");
        }
        {
            KeyValueStore store(path.string(), false, "", 0);
            expect(store.get("color") == "red", "partition state replays after compaction and append");
        }
        const auto before = read_file(path);
        expect_runtime_error([&]() { KeyValueStore store(path.string(), false, "", 1); }, "wrong partition refuses saved log");
        expect_runtime_error([&]() { KeyValueStore store(path.string()); }, "partition log requires explicit partition");
        expect(read_file(path) == before, "wrong configuration leaves saved log untouched");

        for (const auto &contents : {std::string("SET \"color\" \"blue\"\n"),
                                    std::string("PARTITION 0 unknown-routing\n"),
                                    std::string("PARTITION 0 fnv1a64-mod2-v1"),
                                    header + "SET \"full name\" \"Durval\"\n"})
        {
            write_file(path, contents);
            expect_runtime_error([&]() { KeyValueStore store(path.string(), false, "", 0); },
                                 "incompatible, incomplete or wrong-key history is rejected");
            expect(read_file(path) == contents, "rejected history remains untouched");
        }
        std::filesystem::remove(path);
    }

    void test_partition_numbered_history()
    {
        const auto path = test_file("partition_numbered");
        std::filesystem::remove(path);
        {
            KeyValueStore store(path.string(), true, "group-1", 1);
            expect(store.apply_replica_record("1 SET \"full name\" \"Durval\"") == 1, "peer applies owned key");
            const auto before = read_file(path);
            expect_runtime_error([&]() { store.apply_replica_record("2 SET \"color\" \"blue\""); },
                                 "peer cannot bypass partition ownership");
            expect(store.last_sequence() == 1 && read_file(path) == before, "wrong peer record does not advance history");
            expect(store.apply_replica_record("2 SET \"full name\" \"updated\"") == 2, "valid peer progress still works");
        }
        {
            KeyValueStore store(path.string(), true, "group-1", 1);
            expect(store.last_sequence() == 2 && store.get("full name") == "updated", "partition and numbered replay compose");
            expect(store.history_records()[0] == "1 SET \"full name\" \"Durval\"", "metadata is not a numbered mutation");
            expect(store.apply_replica_record("1 SET \"full name\" \"Durval\"") == 1, "duplicate verification survives restart");
        }
        const auto before = read_file(path);
        expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "wrong-group", 1); }, "history ID still checked");
        expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "group-1", 0); }, "numbered log partition still checked");
        expect(read_file(path) == before, "wrong partition/group does not append metadata");
        write_file(path, "PARTITION 1 fnv1a64-mod2-v1\n");
        expect_runtime_error([&]() { KeyValueStore store(path.string(), true, "group-1", 1); }, "missing HISTORY after partition fails");
        std::filesystem::remove(path);
    }
    void test_layout_owned_history()
    {
        const auto path = test_file("layout_owned_history");
        std::filesystem::remove(path);
        std::vector<GroupDefinition> groups{
            {"group-0", "layout-history-0", 6380, 6381, path.string(), path.string() + ".replica"},
            {"group-1", "layout-history-1", 6382, 6383, path.string() + ".p1", path.string() + ".r1"},
            {"group-2", "layout-history-2", 6384, 6385, path.string() + ".p2", path.string() + ".r2"},
        };
        const ClusterLayout layout("fnv1a64-fmix64-ring-v1", 64, groups);
        std::string owned, wrong;
        for (int i = 0; owned.empty() || wrong.empty(); ++i)
        {
            const auto key = "layout-key-" + std::to_string(i);
            if (layout.owner(key).id == "group-0") owned = key;
            else wrong = key;
        }
        std::string identity;
        {
            KeyValueStore store(path.string(), layout, "group-0");
            identity = store.ownership_header();
            expect(read_file(path) == identity + "\nHISTORY layout-history-0\n", "layout identity precedes numbered history");
            store.set(owned, "saved");
            const auto before = read_file(path);
            expect(process_command("SET \"" + wrong + "\" \"wrong\"", store).response.starts_with("ERROR WRONG_PARTITION"),
                   "command processor enforces ring ownership");
            expect_runtime_error([&] { store.apply_replica_record("2 SET \"" + wrong + "\" \"wrong\""); },
                                 "peer cannot bypass layout ownership");
            expect(read_file(path) == before, "wrong owner leaves history untouched");
        }
        const auto saved = read_file(path);
        {
            const ClusterLayout reordered("fnv1a64-fmix64-ring-v1", 64, {groups[2], groups[0], groups[1]});
            KeyValueStore store(path.string(), reordered, "group-0");
            expect(store.ownership_header() == identity && store.get(owned) == "saved" && store.last_sequence() == 1,
                   "ring reordering preserves canonical identity and replay");
            expect(store.apply_replica_record("1 SET \"" + owned + "\" \"saved\"") == 1,
                   "duplicate verification still works for layout-managed history");
        }
        const ClusterLayout changed("fnv1a64-fmix64-ring-v1", 32, groups);
        expect_runtime_error([&] { KeyValueStore store(path.string(), changed, "group-0"); }, "reject changed virtual count on restart");
        expect_runtime_error([&] { KeyValueStore store(path.string(), layout, "group-1"); }, "reject different group on restart");
        expect_runtime_error([&] { KeyValueStore store(path.string(), true, "layout-history-0"); }, "layout log cannot bypass identity through legacy mode");
        expect(read_file(path) == saved, "startup mismatch does not modify saved history");
        write_file(path, identity + "\n");
        expect_runtime_error([&] { KeyValueStore store(path.string(), layout, "group-0"); }, "layout header requires complete HISTORY");
        std::filesystem::remove(path);

        // Existing 7A logs may use the matching two-group modulo layout without
        // rewriting their prefix. Ring adoption still requires actual migration.
        {
            KeyValueStore store(path.string(), true, "layout-history-0", 0);
            store.set("color", "blue");
        }
        const auto legacy = read_file(path);
        const ClusterLayout modulo("fnv1a64-mod-v1", 1, {groups[0], groups[1]});
        {
            KeyValueStore store(path.string(), modulo, "group-0");
            expect(store.get("color") == "blue", "matching modulo layout opens legacy partition log");
        }
        expect(read_file(path) == legacy, "legacy prefix is preserved rather than silently upgraded");
        expect_runtime_error([&] { KeyValueStore store(path.string(), layout, "group-0"); }, "ring cannot reinterpret legacy modulo data");
        std::filesystem::remove(path);
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
    test_layout_owned_history();
    test_partition_ownership();
    test_partition_numbered_history();
    test_history_header();
    test_numbered_history();
    test_invalid_numbered_history();
    test_concurrent_numbered_history();
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
    test_failed_compaction_leaves_log_usable();
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
