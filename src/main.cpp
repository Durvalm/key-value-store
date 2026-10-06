#include "key_value_store.h"
#include "command_processor.h"
#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    const std::string file_path = argc > 1 ? argv[1] : "kv_store.log";
    KeyValueStore store(file_path);
    std::string line;

    std::cout << "Key Value Store\n";
    std::cout << "Type HELP to see available commands.\n";

    while (true)
    {
        std::cout << "> ";
        if (!std::getline(std::cin, line))
        {
            // Input ended, such as when the user presses Ctrl+D.
            break;
        }
        CommandResult result = process_command(line, store);
        std::cout << result.response;
        if (result.close_requested)
        {
            break;
        }
    }

    return 0;
}
