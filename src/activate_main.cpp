#include "layout_activation.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        std::cerr << "Usage: kv_activate <source-layout> <destination-layout> <active-layout>\n"
                     "Stop all clients and source nodes. Restart destinations after transfer; keep traffic paused.\n";
        return 1;
    }
    try
    {
        if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Could not configure socket error handling");
        const auto keys = activate_layout(argv[1], argv[2], argv[3]);
        std::cout << "Verified " << keys << " live keys; published active layout to " << argv[3] << '\n'
                  << "Restart all clients with --layout " << argv[3] << "; then resume traffic.\n"
                     "Keep source nodes stopped and retain their original logs.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << "Activation failed: " << error.what() << '\n';
        return 1;
    }
}
