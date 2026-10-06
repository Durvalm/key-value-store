#include "command_processor.h"

#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    bool read_quoted(std::istringstream &input, std::string &result)
    {
        input >> std::ws;

        if (input.peek() != '"')
        {
            return false;
        }

        return static_cast<bool>(input >> std::quoted(result));
    }

    bool reached_end(std::istringstream &input)
    {
        input >> std::ws;
        return input.eof();
    }

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

CommandResult process_command(const std::string &line, KeyValueStore &store)
{
    if (line.size() > max_request_size)
    {
        return request_too_large_result();
    }

    if (line.find_first_of("\r\n") != std::string::npos)
    {
        return error_result("INVALID_REQUEST", "Request must contain exactly one line");
    }

    std::istringstream input(line);
    std::string command;

    input >> command;

    if (command.empty())
    {
        return error_result("EMPTY_REQUEST", "Request must not be empty");
    }

    try
    {
        if (command == "SET")
        {
            std::string key;
            std::string value;

            if (!read_quoted(input, key) ||
                !read_quoted(input, value) ||
                !reached_end(input) ||
                key.empty() ||
                value.empty())
            {
                return error_result(
                    "INVALID_ARGUMENT",
                    "Usage: SET \"<key>\" \"<value>\"");
            }

            store.set(key, value);
            return {"OK\n", false};
        }
        else if (command == "GET")
        {
            std::string key;

            if (!read_quoted(input, key) || !reached_end(input) || key.empty())
            {
                return error_result("INVALID_ARGUMENT", "Usage: GET \"<key>\"");
            }

            const std::optional<std::string> value = store.get(key);
            return value.has_value() ? value_result(value.value()) : CommandResult{"NOT_FOUND\n", false};
        }
        else if (command == "DELETE")
        {
            std::string key;

            if (!read_quoted(input, key) || !reached_end(input) || key.empty())
            {
                return error_result("INVALID_ARGUMENT", "Usage: DELETE \"<key>\"");
            }

            const bool removed = store.remove(key);
            return {removed ? "INTEGER 1\n" : "INTEGER 0\n", false};
        }
        else if (command == "SIZE")
        {
            if (!reached_end(input))
            {
                return error_result("INVALID_ARGUMENT", "Usage: SIZE");
            }

            return {"INTEGER " + std::to_string(store.size()) + "\n", false};
        }
        else if (command == "HELP")
        {
            if (!reached_end(input))
            {
                return error_result("INVALID_ARGUMENT", "Usage: HELP");
            }

            return {
                "COMMANDS \"SET GET DELETE EXISTS SIZE COMPACT HELP EXIT\"\n",
                false};
        }
        else if (command == "EXISTS")
        {
            std::string key;

            if (!read_quoted(input, key) || !reached_end(input) || key.empty())
            {
                return error_result("INVALID_ARGUMENT", "Usage: EXISTS \"<key>\"");
            }

            return {store.contains(key) ? "INTEGER 1\n" : "INTEGER 0\n", false};
        }
        else if (command == "EXIT")
        {
            if (!reached_end(input))
            {
                return error_result("INVALID_ARGUMENT", "Usage: EXIT");
            }

            return {"BYE\n", true};
        }
        else if (command == "COMPACT")
        {
            if (!reached_end(input))
            {
                return error_result("INVALID_ARGUMENT", "Usage: COMPACT");
            }

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
