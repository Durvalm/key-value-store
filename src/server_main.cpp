#include <arpa/inet.h>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint> // std::uint16_t
#include <cstdio>  // perror

int main()
{
    constexpr std::uint16_t server_port = 6380;

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

    const int client_socket = ::accept(socket_server, nullptr, nullptr);

    if (client_socket == -1)
    {
        perror("accept");
        ::close(socket_server);
        return EXIT_FAILURE;
    }

    char buffer[1024];
    const ssize_t bytes_received =
        ::recv(client_socket, buffer, sizeof(buffer), 0);

    if (bytes_received == -1)
    {
        perror("recv");
        ::close(client_socket);
        ::close(socket_server);
        return EXIT_FAILURE;
    }

    if (bytes_received == 0)
    {
        std::cout << "Client disconnected without sending data.\n";
        ::close(client_socket);
        ::close(socket_server);
        return EXIT_SUCCESS;
    }

    const std::string request(
        buffer,
        static_cast<std::size_t>(bytes_received));

    const std::string response =
        request == "PING\n" ? "PONG\n" : "ERROR\n";

    const ssize_t bytes_sent =
        ::send(client_socket, response.data(), response.size(), 0);

    if (bytes_sent == -1)
    {
        perror("send");
        ::close(client_socket);
        ::close(socket_server);
        return EXIT_FAILURE;
    }

    ::close(client_socket);
    ::close(socket_server);

    return EXIT_SUCCESS;
}