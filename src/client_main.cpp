#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

    int run_client(const std::string &server_address, std::uint16_t port)
    {

        const int client_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (client_socket == -1)
        {
            std::perror("socket");
            return EXIT_FAILURE;
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
            return EXIT_FAILURE;
        }

        if (conversion_result == -1)
        {
            std::perror("inet_pton");
            ::close(client_socket);
            return EXIT_FAILURE;
        }

        if (::connect(
                client_socket,
                reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == -1)
        {
            std::perror("connect");
            ::close(client_socket);
            return EXIT_FAILURE;
        }

        std::cout << "Connected to "
                  << server_address << ':'
                  << port << '\n';

        std::string pending_data;
        std::string line;

        while (std::getline(std::cin, line))
        {
            const std::string request = line + '\n';

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
} // namespace

int main(int argc, char *argv[])
{
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
