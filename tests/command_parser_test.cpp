#include "command_parser.h"
#include "partition.h"

#include <cstdlib>
#include <iostream>
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

    void test_key_extraction_and_partition()
    {
        for (const std::string line : {
                 "SET \"full name\" \"Durval\"", "SET \"full name\" \"another value\"",
                 "GET \"full name\"", "DELETE \"full name\"", "EXISTS \"full name\""})
        {
            const auto command = parse_command(line);
            expect(command.error_response.empty(), "valid keyed command: " + line);
            expect(command.key == "full name", "extract the complete key including spaces");
            expect(partition_for_key(command.key) == 1, "all operations on one key select the same owner");
        }
        const auto color = parse_command("SET \"color\" \"full name\"");
        expect(color.key == "color" && color.value == "full name", "distinguish key from value");
        expect(partition_for_key(color.key) == 0, "route by the key, not the value");

        const auto escaped = parse_command("  SET \"a\\\"b\\\\c\" \"value with spaces\" \t");
        expect(escaped.error_response.empty() && escaped.name == "SET" &&
               escaped.key == "a\"b\\c" && escaped.value == "value with spaces",
               "decode escaped quotes/backslashes using the server quoting rules");
        const auto utf8 = parse_command("GET \"caf\xc3\xa9\"");
        expect(utf8.error_response.empty() && utf8.key == "caf\xc3\xa9" &&
               partition_for_key(utf8.key) == 1, "preserve UTF-8 key bytes for routing");
    }

    void test_commands_without_keys()
    {
        for (const std::string name : {"HELP", "EXIT", "SIZE", "COMPACT"})
        {
            const auto command = parse_command(name);
            expect(command.error_response.empty() && command.name == name &&
                   command.key.empty() && command.value.empty(),
                   "recognize a valid non-keyed command: " + name);
        }
    }

    void test_malformed_commands()
    {
        for (const std::string line : {
                 "SET name value", "SET \"name\"", "SET \"name\" \"\"", "SET \"\" \"value\"",
                 "GET name", "GET \"\"", "GET \"unterminated", "GET \"name\" extra",
                 "DELETE", "EXISTS \"\"", "SET \"name\" \"value\" extra",
                 "HELP extra", "EXIT extra", "SIZE extra", "COMPACT extra"})
        {
            const auto command = parse_command(line);
            expect(command.error_response.starts_with("ERROR INVALID_ARGUMENT ") &&
                   command.error_response.ends_with('\n'), "reject malformed command: " + line);
            expect(command.key.empty(), "invalid input does not expose a routable key");
        }
        expect(parse_command("  \t").error_response.starts_with("ERROR EMPTY_REQUEST "),
               "reject empty input");
        expect(parse_command("UNKNOWN \"name\"").error_response.starts_with("ERROR UNKNOWN_COMMAND "),
               "reject unknown commands");
        expect(parse_command("get \"name\"").error_response.starts_with("ERROR UNKNOWN_COMMAND "),
               "preserve case-sensitive command names");
        for (const std::string line : {"GET \"name\"\n", "GET \"na\rme\""})
            expect(parse_command(line).error_response.starts_with("ERROR INVALID_REQUEST "),
                   "reject raw line separators");
    }

    void test_size_boundary()
    {
        const std::string value(max_request_size - std::string("SET \"k\" \"\"").size(), 'x');
        const std::string line = "SET \"k\" \"" + value + '"';
        const auto command = parse_command(line);
        expect(line.size() == max_request_size && command.error_response.empty() &&
               command.key == "k" && command.value == value, "accept a full-size valid request");
        expect(parse_command(line + ' ').error_response.starts_with("ERROR REQUEST_TOO_LARGE "),
               "check the size of the entire request, not just the key");
    }
}

int main()
{
    test_key_extraction_and_partition();
    test_commands_without_keys();
    test_malformed_commands();
    test_size_boundary();
    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "All command parsing and partition-selection tests passed.\n";
    return EXIT_SUCCESS;
}
