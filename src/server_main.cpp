#include "command_processor.h"
#include "key_value_store.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    constexpr std::uint16_t server_port = 6380;

    const std::string file_path =
        argc > 1 ? argv[1] : "kv_server.log";

    KeyValueStore store(file_path);

    const int socket_server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (socket_server == -1)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse_address = 1;

    if (::setsockopt(
            socket_server,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1)
    {
        perror("setsockopt");
        ::close(socket_server);
        return EXIT_FAILURE;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_port);
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(
            socket_server,
            reinterpret_cast<const sockaddr *>(&server_address),
            sizeof(server_address)) == -1)
    {
        perror("bind");
        ::close(socket_server);
        return EXIT_FAILURE;
    }

    if (::listen(socket_server, 1) == -1)
    {
        perror("listen");
        ::close(socket_server);
        return EXIT_FAILURE;
    }

    std::cout << "Listening on 127.0.0.1:" << server_port << '\n';

    while (true)
    {
        const int client_socket = ::accept(socket_server, nullptr, nullptr);

        if (client_socket == -1)
        {
            perror("accept");
            break;
        }

        std::string pending_data;
        bool client_connected = true;

        while (client_connected)
        {
            char buffer[1024];
            const ssize_t bytes_received =
                ::recv(client_socket, buffer, sizeof(buffer), 0);

            if (bytes_received == -1)
            {
                std::perror("recv");
                break;
            }

            if (bytes_received == 0)
            {
                std::cout << "Client disconnected.\n";
                break;
            }

            pending_data.append(
                buffer,
                static_cast<std::size_t>(bytes_received));

            std::size_t newline_position;

            while ((newline_position = pending_data.find('\n')) != std::string::npos)
            {
                // process_command expects the request without its framing newline.
                const std::string request =
                    pending_data.substr(0, newline_position);

                // Remove the request and its newline from pending_data.
                pending_data.erase(0, newline_position + 1);

                const CommandResult result =
                    process_command(request, store);

                const ssize_t bytes_sent =
                    ::send(client_socket, result.response.data(), result.response.size(), 0);

                if (bytes_sent == -1)
                {
                    std::perror("send");
                    client_connected = false;
                    break;
                }
            }
        }

        ::close(client_socket);
    }

    ::close(socket_server);

    return EXIT_SUCCESS;
}
