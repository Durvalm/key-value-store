#include "key_value_store.h"

#include <iostream>
#include <sstream>
#include <string>

int main()
{
    KeyValueStore cache;
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

        std::istringstream input(line);
        std::string command;

        input >> command;

        if (command.empty())
        {
            continue;
        }

        if (command == "SET")
        {
            std::string key;
            std::string value;

            input >> key;
            std::getline(input >> std::ws, value);

            if (key.empty() || value.empty())
            {
                std::cout << "Usage: Set <key> <value>\n";
                continue;
            }
            cache.set(key, value);
            std::cout << "OK\n";
        }
        else if (command == "GET")
        {
            std::string key;
            input >> key;

            if (key.empty())
            {
                std::cout << "Usage: GET <key>\n";
                continue;
            }

            std::optional<std::string> value = cache.get(key);

            if (value.has_value())
            {
                std::cout << value.value() << '\n';
            }
            else
            {
                std::cout << "(nil)\n";
            }
        }
        else if (command == "DELETE")
        {
            std::string key;
            input >> key;

            if (key.empty())
            {
                std::cout << "Usage: DELETE <key>\n";
                continue;
            }

            bool removed = cache.remove(key);
            std::cout << (removed ? "1" : "0") << '\n';
        }
        else if (command == "SIZE")
        {
            std::cout << cache.size() << '\n';
        }
        else if (command == "HELP")
        {
            std::cout
                << "SET <key> <value>\n"
                << "GET <key>\n"
                << "DELETE <key>\n"
                << "SIZE\n"
                << "HELP\n"
                << "EXIT\n";
        }
        else if (command == "EXIT")
        {
            break;
        }
        else
        {
            std::cout << "Unknown command. Type HELP.\n";
        }
    }

    return 0;
}