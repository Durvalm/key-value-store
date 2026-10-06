#include "command_processor.h"
#include "key_value_store.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace
{
    constexpr std::uint16_t server_port = 6380;
    constexpr unsigned int default_idle_timeout_seconds = 30;
    constexpr unsigned int max_idle_timeout_seconds = 3600;

    volatile std::sig_atomic_t shutdown_requested = 0;

    void request_shutdown(int)
    {
        shutdown_requested = 1;
    }

    bool install_signal_handlers()
    {
        struct sigaction shutdown_action{};
        shutdown_action.sa_handler = request_shutdown;
        sigemptyset(&shutdown_action.sa_mask);
        shutdown_action.sa_flags = 0;

        if (::sigaction(SIGINT, &shutdown_action, nullptr) == -1 ||
            ::sigaction(SIGTERM, &shutdown_action, nullptr) == -1)
        {
            return false;
        }

        struct sigaction ignored_action{};
        ignored_action.sa_handler = SIG_IGN;
        sigemptyset(&ignored_action.sa_mask);
        ignored_action.sa_flags = 0;

        return ::sigaction(SIGPIPE, &ignored_action, nullptr) != -1;
    }

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

    bool parse_idle_timeout(const std::string &text, unsigned int &seconds)
    {
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }

        try
        {
            const unsigned long value = std::stoul(text);

            if (value == 0 || value > max_idle_timeout_seconds)
            {
                return false;
            }

            seconds = static_cast<unsigned int>(value);
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
                    if (shutdown_requested)
                    {
                        return false;
                    }

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

    void handle_client(int client_socket, KeyValueStore &store, unsigned int idle_timeout_seconds)
    {

        timeval receive_timeout{};
        receive_timeout.tv_sec = static_cast<decltype(receive_timeout.tv_sec)>(idle_timeout_seconds);

        if (::setsockopt(
                client_socket,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &receive_timeout,
                sizeof(receive_timeout)) == -1)
        {
            std::perror("setsockopt SO_RCVTIMEO");
            return;
        }

        std::string pending_data;
        bool client_connected = true;

        while (client_connected && !shutdown_requested)
        {
            char buffer[1024];
            const ssize_t bytes_received =
                ::recv(client_socket, buffer, sizeof(buffer), 0);

            if (bytes_received == -1)
            {
                if (errno == EINTR)
                {
                    if (shutdown_requested)
                    {
                        break;
                    }

                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::cerr << "Client disconnected after "
                              << idle_timeout_seconds
                              << " seconds without a complete request.\n";
                    break;
                }

                std::perror("recv");
                break;
            }

            if (bytes_received == 0)
            {
                if (!pending_data.empty())
                {
                    std::cerr << "Warning: discarding incomplete request from disconnected client.\n";
                }
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

                if (!send_all(client_socket, result.response))
                {
                    client_connected = false;
                    break;
                }

                if (result.close_requested)
                {
                    client_connected = false;
                    break;
                }
            }

            if (client_connected && pending_data.size() > max_request_size)
            {
                send_all(client_socket, request_too_large_result().response);
                client_connected = false;
            }
        }
    }

    int run_server(
        const std::string &file_path,
        std::uint16_t port,
        unsigned int idle_timeout_seconds)
    {
        KeyValueStore store(file_path);

        const int socket_server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (socket_server == -1)
        {
            std::perror("socket");
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
            std::perror("setsockopt");
            ::close(socket_server);
            return EXIT_FAILURE;
        }

        sockaddr_in server_address{};
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port);
        server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::bind(
                socket_server,
                reinterpret_cast<const sockaddr *>(&server_address),
                sizeof(server_address)) == -1)
        {
            std::perror("bind");
            ::close(socket_server);
            return EXIT_FAILURE;
        }

        if (::listen(socket_server, 1) == -1)
        {
            std::perror("listen");
            ::close(socket_server);
            return EXIT_FAILURE;
        }

        std::cout << "Listening on 127.0.0.1:" << port << '\n';

        while (!shutdown_requested)
        {
            const int client_socket = ::accept(socket_server, nullptr, nullptr);

            if (client_socket == -1)
            {
                if (errno == EINTR)
                {
                    if (shutdown_requested)
                    {
                        break;
                    }

                    continue;
                }

                std::perror("accept");
                ::close(socket_server);
                return EXIT_FAILURE;
            }

            handle_client(client_socket, store, idle_timeout_seconds);

            ::close(client_socket);
        }

        ::close(socket_server);
        std::cout << "Server shut down.\n";
        return EXIT_SUCCESS;
    }
} // namespace

int main(int argc, char *argv[])
{
    if (argc > 4)
    {
        std::cerr << "Usage: kv_server [persistence-file] [port] [idle-timeout-seconds]\n";
        return EXIT_FAILURE;
    }

    const std::string file_path =
        argc > 1 ? argv[1] : "kv_server.log";

    std::uint16_t port = server_port;
    unsigned int idle_timeout_seconds = default_idle_timeout_seconds;

    if (argc >= 3 && !parse_port(argv[2], port))
    {
        std::cerr << "Invalid port: expected an integer from 1 to 65535.\n";
        return EXIT_FAILURE;
    }

    if (argc == 4 && !parse_idle_timeout(argv[3], idle_timeout_seconds))
    {
        std::cerr << "Invalid idle timeout: expected an integer from 1 to "
                  << max_idle_timeout_seconds << " seconds.\n";
        return EXIT_FAILURE;
    }

    if (!install_signal_handlers())
    {
        std::perror("sigaction");
        return EXIT_FAILURE;
    }

    try
    {
        return run_server(file_path, port, idle_timeout_seconds);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Server startup failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
