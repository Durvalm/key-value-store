#include "command_parser.h"
#include "partition.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <stdexcept>

#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace
{
    constexpr std::uint16_t default_server_port = 6380;

    bool parse_port(const std::string &text, std::uint16_t &port)
    {
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }

        try
        {
            const unsigned long value = std::stoul(text);

            if (value == 0 || value > 65535)
            {
                return false;
            }

            port = static_cast<std::uint16_t>(value);
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    bool send_all(int socket, const std::string &data)
    {
        std::size_t total_sent = 0;

        while (total_sent < data.size())
        {
            const ssize_t bytes_sent = ::send(
                socket,
                data.data() + total_sent,
                data.size() - total_sent,
                0);

            if (bytes_sent == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                std::perror("send");
                return false;
            }

            if (bytes_sent == 0)
            {
                return false;
            }

            total_sent += static_cast<std::size_t>(bytes_sent);
        }

        return true;
    }

    bool receive_line(
        int socket,
        std::string &pending_data,
        std::string &response)
    {
        while (true)
        {
            const std::size_t newline_position = pending_data.find('\n');

            if (newline_position != std::string::npos)
            {
                response = pending_data.substr(0, newline_position);
                pending_data.erase(0, newline_position + 1);
                return true;
            }

            char buffer[1024];

            const ssize_t bytes_received =
                ::recv(socket, buffer, sizeof(buffer), 0);

            if (bytes_received == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                std::perror("recv");
                return false;
            }

            if (bytes_received == 0)
            {
                return false;
            }

            pending_data.append(
                buffer,
                static_cast<std::size_t>(bytes_received));
        }
    }

    // The caller owns the returned socket and must close it.
    int connect_to_server(const std::string &server_address, std::uint16_t port)
    {
        const int client_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (client_socket == -1)
        {
            std::perror("socket");
            return -1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        const int conversion_result = ::inet_pton(
            AF_INET,
            server_address.c_str(),
            &address.sin_addr);

        if (conversion_result == 0)
        {
            std::cerr << "Invalid IPv4 address: "
                      << server_address << '\n';
            ::close(client_socket);
            return -1;
        }

        if (conversion_result == -1)
        {
            std::perror("inet_pton");
            ::close(client_socket);
            return -1;
        }

        try
        {
            // Bound the connection wait, then restore ordinary blocking I/O.
            const int flags = ::fcntl(client_socket, F_GETFL, 0);
            if (flags == -1 || ::fcntl(client_socket, F_SETFL, flags | O_NONBLOCK) == -1)
                throw std::runtime_error("Could not set socket flags");
            if (::connect(client_socket, reinterpret_cast<const sockaddr *>(&address),
                          sizeof(address)) == -1)
            {
                if (errno != EINPROGRESS)
                    throw std::runtime_error("Connection failed");
                pollfd connection{client_socket, POLLOUT, 0};
                if (::poll(&connection, 1, 5000) <= 0)
                    throw std::runtime_error("Connection wait failed or timed out");
                int error = 0;
                socklen_t length = sizeof(error);
                if (::getsockopt(client_socket, SOL_SOCKET, SO_ERROR, &error, &length) == -1 || error != 0)
                    throw std::runtime_error("Connection failed");
            }
            if (::fcntl(client_socket, F_SETFL, flags) == -1)
                throw std::runtime_error("Could not restore socket flags");
            timeval timeout{};
            timeout.tv_sec = 5;
            if (::setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
                ::setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1)
                throw std::runtime_error("Could not set socket timeouts");
        }
        catch (const std::exception &error)
        {
            std::cerr << server_address << ':' << port << ": " << error.what() << '\n';
            ::close(client_socket);
            return -1;
        }
        return client_socket;
    }

    int run_client(const std::string &server_address, std::uint16_t port)
    {
        const int client_socket = connect_to_server(server_address, port);
        if (client_socket == -1)
            return EXIT_FAILURE;

        std::cout << "Connected to "
                  << server_address << ':'
                  << port << '\n';

        std::string pending_data;
        std::string line;

        while (std::getline(std::cin, line))
        {
            const std::string request = line + '\n';
            const ParsedCommand parsed = parse_command(line);
            if (!parsed.error_response.empty())
            {
                std::cout << parsed.error_response;
                continue;
            }

            if (!send_all(client_socket, request))
            {
                ::close(client_socket);
                return EXIT_FAILURE;
            }

            std::string response;

            if (!receive_line(client_socket, pending_data, response))
            {
                std::cerr << "Server disconnected before sending a complete response.\n";
                ::close(client_socket);
                return EXIT_FAILURE;
            }

            std::cout << response << '\n';

            if (response == "BYE")
            {
                break;
            }
        }

        ::close(client_socket);
        return EXIT_SUCCESS;
    }

    int run_cluster_client(const std::string &server_address, const std::uint16_t ports[2])
    {
        int exit_status = EXIT_SUCCESS;
        std::string line;
        while (std::getline(std::cin, line))
        {
            const ParsedCommand parsed = parse_command(line);
            if (!parsed.error_response.empty())
            {
                std::cout << parsed.error_response;
                continue;
            }
            if (parsed.name == "EXIT")
            {
                std::cout << "BYE\n";
                break;
            }
            if (parsed.name == "HELP")
            {
                std::cout << "Commands: SET \"key\" \"value\", GET \"key\", DELETE \"key\", "
                             "EXISTS \"key\", HELP, EXIT\n";
                continue;
            }
            if (parsed.key.empty())
            {
                std::cout << "ERROR UNSUPPORTED \"SIZE and COMPACT are unsupported in cluster mode\"\n";
                continue;
            }

            const std::size_t partition = partition_for_key(parsed.key);
            const int client_socket = connect_to_server(server_address, ports[partition]);
            if (client_socket == -1)
            {
                std::cout << "ERROR PARTITION_UNAVAILABLE \"Could not connect to partition "
                          << partition << "\"\n";
                exit_status = EXIT_FAILURE;
                continue;
            }

            // Each exchange owns its socket and receive buffer. Never retry a write
            // automatically: a lost response does not mean the write was not applied.
            std::string pending_data;
            std::string response;
            const bool success = send_all(client_socket, line + '\n') &&
                                 receive_line(client_socket, pending_data, response);
            ::close(client_socket);
            if (success)
                std::cout << response << '\n';
            else
            {
                std::cout << "ERROR REQUEST_UNCONFIRMED \"No complete response; a write may have been applied\"\n";
                exit_status = EXIT_FAILURE;
            }
        }
        return exit_status;
    }
} // namespace

int main(int argc, char *argv[])
{
    // A disconnected peer should produce a send error rather than terminate us.
    std::signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && std::string(argv[1]) == "--cluster")
    {
        std::uint16_t ports[2]{};
        if (argc != 5 || !parse_port(argv[3], ports[0]) || !parse_port(argv[4], ports[1]) ||
            ports[0] == ports[1])
        {
            std::cerr << "Usage: kv_client --cluster <IPv4-address> <partition-0-port> <partition-1-port>\n"
                         "Ports must be distinct integers from 1 to 65535.\n";
            return EXIT_FAILURE;
        }
        return run_cluster_client(argv[2], ports);
    }
    if (argc > 3)
    {
        std::cerr << "Usage: kv_client [server-address] [port]\n";
        return EXIT_FAILURE;
    }

    const std::string server_address = argc > 1 ? argv[1] : "127.0.0.1";

    std::uint16_t port = default_server_port;

    if (argc == 3 && !parse_port(argv[2], port))
    {
        std::cerr << "Invalid port: expected an integer from 1 to 65535.\n";
        return EXIT_FAILURE;
    }

    try
    {
        return run_client(server_address, port);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Client failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
