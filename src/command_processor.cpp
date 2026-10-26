#include "command_processor.h"

#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    CommandResult error_result(const std::string &code, const std::string &message)
    {
        std::ostringstream response;
        response << "ERROR " << code << ' ' << std::quoted(message) << '\n';
        return {response.str(), false};
    }

    CommandResult value_result(const std::string &value)
    {
        std::ostringstream response;
        response << "VALUE " << std::quoted(value) << '\n';
        return {response.str(), false};
    }
} // namespace

CommandResult request_too_large_result()
{
    return error_result("REQUEST_TOO_LARGE", "Request exceeds 4096 bytes");
}

CommandResult process_command(const std::string &line, KeyValueStore &store,
                              bool read_only, bool allow_compaction)
{
    const ParsedCommand parsed = parse_command(line);
    if (!parsed.error_response.empty())
        return {parsed.error_response, false};

    const std::string &command = parsed.name;
    const std::string &key = parsed.key;
    const std::string &value = parsed.value;

    try
    {
        if (command == "SET")
        {
            if (read_only)
                return error_result("READ_ONLY", "Client writes are disabled on this connection");

            store.set(key, value);
            return {"OK\n", false, true};
        }
        else if (command == "GET")
        {
            const std::optional<std::string> value = store.get(key);
            return value.has_value() ? value_result(value.value()) : CommandResult{"NOT_FOUND\n", false};
        }
        else if (command == "DELETE")
        {
            if (read_only)
                return error_result("READ_ONLY", "Client writes are disabled on this connection");

            const bool removed = store.remove(key);
            return {removed ? "INTEGER 1\n" : "INTEGER 0\n", false, removed};
        }
        else if (command == "SIZE")
        {
            return {"INTEGER " + std::to_string(store.size()) + "\n", false};
        }
        else if (command == "HELP")
        {
            return {
                "COMMANDS \"SET GET DELETE EXISTS SIZE COMPACT HELP EXIT\"\n",
                false};
        }
        else if (command == "EXISTS")
        {
            return {store.contains(key) ? "INTEGER 1\n" : "INTEGER 0\n", false};
        }
        else if (command == "EXIT")
        {
            return {"BYE\n", true};
        }
        else if (command == "COMPACT")
        {
            if (!allow_compaction)
                return error_result("COMPACTION_DISABLED", "Compaction is disabled during replication experiments");

            store.compact();
            return {"OK\n", false};
        }
    }
    catch (const std::invalid_argument &error)
    {
        return error_result("INVALID_ARGUMENT", error.what());
    }
    catch (const std::exception &error)
    {
        return error_result("INTERNAL", error.what());
    }

    return error_result("UNKNOWN_COMMAND", "Unknown command");
}
