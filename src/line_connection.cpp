#include "line_connection.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

// One request is outstanding at a time. The caller serializes each exchange.
std::string exchange_message(int socket, const std::string &request, std::size_t response_limit)
{
    std::size_t sent = 0;
    while (sent < request.size())
    {
        const ssize_t count = ::send(socket, request.data() + sent, request.size() - sent, 0);
        if (count == -1)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Could not send request");
        }
        if (count == 0)
            throw std::runtime_error("Socket send made no progress");
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    while (response.find('\n') == std::string::npos)
    {
        char buffer[128];
        const ssize_t count = ::recv(socket, buffer, sizeof(buffer), 0);
        if (count == -1)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Could not receive acknowledgment");
        }
        if (count == 0)
            throw std::runtime_error("Peer disconnected before acknowledgment");
        response.append(buffer, static_cast<std::size_t>(count));
        if (response.size() > response_limit)
            throw std::runtime_error("Response exceeds limit");
    }
    if (response.find('\n') != response.size() - 1 || response.find('\r') != std::string::npos)
        throw std::runtime_error("Expected exactly one response line");
    return response;
}

int connect_loopback(std::uint16_t port)
{
    const int socket =
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (socket == -1)
        throw std::runtime_error("Could not create socket");

    try
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // Temporarily make connect nonblocking.
        const int flags = ::fcntl(socket, F_GETFL, 0);

        if (flags == -1 ||
            ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            throw std::runtime_error("Could not set socket flags");
        }

        if (::connect(
                socket,
                reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == -1)
        {
            if (errno != EINPROGRESS)
                throw std::runtime_error(std::strerror(errno));

            // Wait up to five seconds for connection completion.
            pollfd connection{socket, POLLOUT, 0};
            const int ready = ::poll(&connection, 1, 5000);

            if (ready == -1)
                throw std::runtime_error("Connection polling failed");

            if (ready == 0)
                throw std::runtime_error("Connection timed out");

            // Readiness can mean success or failure. Check which.
            int error = 0;
            socklen_t length = sizeof(error);

            if (::getsockopt(
                    socket, SOL_SOCKET, SO_ERROR,
                    &error, &length) == -1)
            {
                throw std::runtime_error("Could not check connection");
            }

            if (error != 0)
                throw std::runtime_error(std::strerror(error));
        }

        // Ordinary sends and receives will use blocking mode.
        if (::fcntl(socket, F_SETFL, flags) == -1)
            throw std::runtime_error("Could not restore socket flags");

        timeval timeout{};
        timeout.tv_sec = 5;

        if (::setsockopt(
                socket, SOL_SOCKET, SO_RCVTIMEO,
                &timeout, sizeof(timeout)) == -1 ||
            ::setsockopt(
                socket, SOL_SOCKET, SO_SNDTIMEO,
                &timeout, sizeof(timeout)) == -1)
        {
            throw std::runtime_error("Could not set I/O timeouts");
        }

        return socket;
    }
    catch (...)
    {
        ::close(socket);
        throw;
    }
}
