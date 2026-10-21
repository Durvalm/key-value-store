#include "replica_connection.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace
{
    // One request is outstanding at a time. The caller serializes each exchange.
    void exchange_message(int socket, const std::string &request,
                          const std::string &expected_response)
    {
        std::size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t count = ::send(socket, request.data() + sent, request.size() - sent, 0);
            if (count == -1)
            {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("Could not send to replica");
            }
            if (count == 0)
                throw std::runtime_error("Replica send made no progress");
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
                throw std::runtime_error("Could not receive replica acknowledgment");
            }
            if (count == 0)
                throw std::runtime_error("Replica disconnected before acknowledgment");
            response.append(buffer, static_cast<std::size_t>(count));
            if (response.size() > 512)
                throw std::runtime_error("Replica acknowledgment exceeds limit");
        }
        if (response != expected_response)
            throw std::runtime_error("Unexpected replica acknowledgment: " + response);
    }
}

int connect_to_replica(std::uint16_t port, const std::string &history_id, std::uint64_t last_sequence)
{
    const int replica_socket =
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (replica_socket == -1)
        throw std::runtime_error("Could not create replica socket");

    try
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // Temporarily make connect nonblocking.
        const int flags = ::fcntl(replica_socket, F_GETFL, 0);

        if (flags == -1 ||
            ::fcntl(replica_socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            throw std::runtime_error("Could not set socket flags");
        }

        if (::connect(
                replica_socket,
                reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == -1)
        {
            if (errno != EINPROGRESS)
                throw std::runtime_error(std::strerror(errno));

            // Wait up to five seconds for connection completion.
            pollfd connection{replica_socket, POLLOUT, 0};
            const int ready = ::poll(&connection, 1, 5000);

            if (ready == -1)
                throw std::runtime_error("Connection polling failed");

            if (ready == 0)
                throw std::runtime_error("Replica connection timed out");

            // Readiness can mean success or failure. Check which.
            int error = 0;
            socklen_t length = sizeof(error);

            if (::getsockopt(
                    replica_socket, SOL_SOCKET, SO_ERROR,
                    &error, &length) == -1)
            {
                throw std::runtime_error("Could not check connection");
            }

            if (error != 0)
                throw std::runtime_error(std::strerror(error));
        }

        // Ordinary sends and receives will use blocking mode.
        if (::fcntl(replica_socket, F_SETFL, flags) == -1)
            throw std::runtime_error("Could not restore socket flags");

        timeval timeout{};
        timeout.tv_sec = 5;

        if (::setsockopt(
                replica_socket, SOL_SOCKET, SO_RCVTIMEO,
                &timeout, sizeof(timeout)) == -1 ||
            ::setsockopt(
                replica_socket, SOL_SOCKET, SO_SNDTIMEO,
                &timeout, sizeof(timeout)) == -1)
        {
            throw std::runtime_error("Could not set replica I/O timeouts");
        }

        // Explicitly mark this as a peer session, rather than an ordinary client.
        exchange_message(
            replica_socket,
            "REPLICA " + history_id + "\n",
            "READY " + history_id + ' ' + std::to_string(last_sequence) + "\n");
        return replica_socket;
    }
    catch (...)
    {
        ::close(replica_socket);
        throw;
    }
}

void send_to_replica(int replica_socket, const std::string &line,
                     const std::string &expected_response)
{
    exchange_message(replica_socket, line + '\n', expected_response);
}
