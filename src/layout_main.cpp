#include "cluster_layout.h"

#include <exception>
#include <iostream>

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: kv_layout <old-layout> <new-layout> <key> [key ...]\n";
        return 1;
    }
    try
    {
        const auto before = read_layout(argv[1]);
        const auto after = read_layout(argv[2]);
        std::cout << "Old: " << before.routing() << ", " << before.groups().size()
                  << " groups, " << before.positions_per_group() << " positions/group\n"
                  << "New: " << after.routing() << ", " << after.groups().size()
                  << " groups, " << after.positions_per_group() << " positions/group\n";
        for (int i = 3; i < argc; ++i)
        {
            const auto &old_owner = before.owner(argv[i]);
            const auto &new_owner = after.owner(argv[i]);
            std::cout << argv[i] << ": " << old_owner.id << " (:" << old_owner.primary_port
                      << ") -> " << new_owner.id << " (:" << new_owner.primary_port << ")\n";
        }
        std::cout << "Preview only: no sockets opened, log files changed, or data migrated.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "Layout failed: " << error.what() << '\n';
        return 1;
    }
}
