#include "command_parser.h"

#include <iomanip>
#include <sstream>

namespace
{
    bool read_quoted(std::istringstream &input, std::string &result)
    {
        input >> std::ws;
        return input.peek() == '"' && static_cast<bool>(input >> std::quoted(result));
    }

    bool reached_end(std::istringstream &input)
    {
        input >> std::ws;
        return input.eof();
    }

    ParsedCommand parse_error(const std::string &code, const std::string &message)
    {
        std::ostringstream response;
        response << "ERROR " << code << ' ' << std::quoted(message) << '\n';
        ParsedCommand result;
        result.error_response = response.str();
        return result;
    }
}

ParsedCommand parse_command(const std::string &line)
{
    if (line.size() > max_request_size)
        return parse_error("REQUEST_TOO_LARGE", "Request exceeds 4096 bytes");
    if (line.find_first_of("\r\n") != std::string::npos)
        return parse_error("INVALID_REQUEST", "Request must contain exactly one line");

    std::istringstream input(line);
    ParsedCommand command;
    input >> command.name;
    if (command.name.empty())
        return parse_error("EMPTY_REQUEST", "Request must not be empty");

    if (command.name == "SET")
    {
        if (!read_quoted(input, command.key) || !read_quoted(input, command.value) ||
            !reached_end(input) || command.key.empty() || command.value.empty())
            return parse_error("INVALID_ARGUMENT", "Usage: SET \"<key>\" \"<value>\"");
    }
    else if (command.name == "GET" || command.name == "DELETE" || command.name == "EXISTS")
    {
        if (!read_quoted(input, command.key) || !reached_end(input) || command.key.empty())
            return parse_error("INVALID_ARGUMENT", "Usage: " + command.name + " \"<key>\"");
    }
    else if (command.name == "HELP" || command.name == "EXIT" ||
             command.name == "SIZE" || command.name == "COMPACT")
    {
        if (!reached_end(input))
            return parse_error("INVALID_ARGUMENT", "Usage: " + command.name);
    }
    else
    {
        return parse_error("UNKNOWN_COMMAND", "Unknown command");
    }
    return command;
}
