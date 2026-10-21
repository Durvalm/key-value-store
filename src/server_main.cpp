#include "command_processor.h"
#include "key_value_store.h"
#include "replica_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace
{
    constexpr std::uint16_t default_server_port = 6380;
    constexpr std::uint16_t default_replica_port = 6381;
    constexpr unsigned int default_idle_timeout_seconds = 120;
    constexpr unsigned int max_idle_timeout_seconds = 3600;
    constexpr std::size_t worker_count = 2;
    constexpr std::size_t pending_connection_capacity = 16;

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

    void handle_client(int client_socket, KeyValueStore &store, unsigned int idle_timeout_seconds,
                       const std::string &role, int replica_socket,
                       std::mutex &replication_mutex, bool &replica_failed,
                       bool &replica_session_used)
    {

        timeval io_timeout{};
        io_timeout.tv_sec = static_cast<decltype(io_timeout.tv_sec)>(idle_timeout_seconds);

        if (::setsockopt(
                client_socket,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &io_timeout,
                sizeof(io_timeout)) == -1)
        {
            std::perror("setsockopt SO_RCVTIMEO");
            return;
        }

        if (::setsockopt(
                client_socket,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &io_timeout,
                sizeof(io_timeout)) == -1)
        {
            std::perror("setsockopt SO_SNDTIMEO");
            return;
        }

        std::string pending_data;
        bool client_connected = true;
        bool peer_connection = false;

        while (client_connected)
        {
            char buffer[1024];
            const ssize_t bytes_received =
                ::recv(client_socket, buffer, sizeof(buffer), 0);

            if (bytes_received == -1)
            {
                if (errno == EINTR)
                {
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

                if (role == "replica" && !peer_connection)
                {
                    std::istringstream handshake(request);
                    std::string command;
                    handshake >> command;
                    if (command == "REPLICA")
                    {
                        std::string history_id;
                        std::string extra;
                        if (request.size() > max_request_size ||
                            request.find_first_of("\r\n") != std::string::npos ||
                            !(handshake >> history_id) || (handshake >> extra))
                        {
                            send_all(client_socket, "ERROR INVALID_HANDSHAKE \"Usage: REPLICA <history-id>\"\n");
                            return;
                        }
                        if (history_id != store.history_id())
                        {
                            send_all(client_socket, "ERROR HISTORY_MISMATCH \"Replica belongs to a different history\"\n");
                            return;
                        }

                        // Validate before claiming the one allowed peer session.
                        std::lock_guard<std::mutex> lock(replication_mutex);
                        if (replica_session_used)
                        {
                            send_all(client_socket, "ERROR SESSION_REJECTED \"Restart replica before opening another peer session\"\n");
                            return;
                        }
                        replica_session_used = true;
                        peer_connection = true;
                        const std::string response = "READY " + store.history_id() + ' ' +
                            std::to_string(store.last_sequence()) + '\n';
                        if (!send_all(client_socket, response))
                            return;
                        continue;
                    }
                }

                CommandResult result;
                if (peer_connection)
                {
                    try
                    {
                        // A peer sends numbered mutations, not ordinary client commands.
                        if (request.size() > max_request_size + 21)
                            throw std::runtime_error("Replication record exceeds limit");
                        const auto sequence = store.apply_replica_record(request);
                        result = {"ACK " + std::to_string(sequence) + '\n', false};
                    }
                    catch (const std::exception &error)
                    {
                        std::cerr << "Replica rejected record: " << error.what() << '\n';
                        result = {"ERROR REPLICATION_RECORD\n", true};
                    }
                }
                else if (role == "primary")
                {
                    // One order for local application, sending, and acknowledgment.
                    // Socket-only locking would still allow local/remote order to differ.
                    std::lock_guard<std::mutex> lock(replication_mutex);
                    result = process_command(request, store, replica_failed, false);
                    if (replica_failed && result.response.starts_with("ERROR READ_ONLY"))
                        result.response = "ERROR REPLICA_UNAVAILABLE \"Replication failed; further writes are disabled\"\n";

                    if (result.mutation_applied)
                    {
                        try
                        {
                            const auto sequence = store.last_sequence();
                            send_to_replica(replica_socket,
                                            std::to_string(sequence) + ' ' + request,
                                            "ACK " + std::to_string(sequence) + '\n');
                        }
                        catch (const std::exception &error)
                        {
                            replica_failed = true;
                            ::shutdown(replica_socket, SHUT_RDWR);
                            std::cerr << "Replication failed: " << error.what() << '\n';
                            result.response = "ERROR REPLICATION_UNCERTAIN \"Write applied locally; replica acknowledgment is unconfirmed\"\n";
                        }
                    }
                    // A persistence failure may have left partial history. Do not
                    // continue admitting writes after it in this experiment.
                    if (result.response.starts_with("ERROR INTERNAL"))
                        replica_failed = true;
                }
                else
                {
                    result = process_command(request, store,
                                             role == "replica" && !peer_connection,
                                             role == "standalone");
                }

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

            // A uint64 sequence plus its separating space adds at most 21 bytes.
            const auto frame_limit = max_request_size + (peer_connection ? 21 : 0);
            if (client_connected && pending_data.size() > frame_limit)
            {
                send_all(client_socket, request_too_large_result().response);
                client_connected = false;
            }
        }
    }

    int run_server(
        const std::string &file_path,
        std::uint16_t port,
        unsigned int idle_timeout_seconds,
        const std::string &role,
        std::uint16_t replica_port,
        const std::string &new_history_id)
    {
        KeyValueStore store(file_path, role != "standalone", new_history_id);

        const int listening_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (listening_socket == -1)
        {
            std::perror("socket");
            return EXIT_FAILURE;
        }

        int reuse_address = 1;

        if (::setsockopt(
                listening_socket,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse_address,
                sizeof(reuse_address)) == -1)
        {
            std::perror("setsockopt");
            ::close(listening_socket);
            return EXIT_FAILURE;
        }

        sockaddr_in server_address{};
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port);
        server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::bind(
                listening_socket,
                reinterpret_cast<const sockaddr *>(&server_address),
                sizeof(server_address)) == -1)
        {
            std::perror("bind");
            ::close(listening_socket);
            return EXIT_FAILURE;
        }

        if (::listen(
                listening_socket,
                static_cast<int>(pending_connection_capacity)) == -1)
        {
            std::perror("listen");
            ::close(listening_socket);
            return EXIT_FAILURE;
        }

        std::cout << "Listening on 127.0.0.1:" << port << '\n';

        // Only the primary opens an outgoing connection. It owns its lifetime.
        int replica_socket = -1;
        if (role == "primary")
        {
            try
            {
                replica_socket = connect_to_replica(replica_port, store.history_id(), store.history_records());
                std::cout << "Connected to replica on 127.0.0.1:" << replica_port << '\n';
            }
            catch (...)
            {
                ::close(listening_socket);
                throw;
            }
        }

        // start the workers
        std::vector<std::thread> workers;
        int exit_status = EXIT_SUCCESS;

        std::queue<int> pending_client_sockets;
        std::mutex connection_state_mutex;
        std::condition_variable queue_condition;
        bool workers_stopping = false;
        std::array<int, worker_count> active_client_sockets{};
        active_client_sockets.fill(-1);
        std::mutex replication_mutex;
        bool replica_failed = false;
        bool replica_session_used = false;

        // Workers share the queue but own one active client socket at a time.
        auto worker_task = [&pending_client_sockets, &connection_state_mutex,
                            &queue_condition, &workers_stopping,
                            &active_client_sockets, &store,
                            &replication_mutex, &replica_failed, &replica_session_used,
                            &role, idle_timeout_seconds, replica_socket](std::size_t worker_index)
        {
            while (true)
            {
                int client_socket;
                {
                    std::unique_lock<std::mutex> lock(connection_state_mutex);

                    queue_condition.wait(lock, [&]()
                                         { return workers_stopping || !pending_client_sockets.empty(); });
                    if (workers_stopping)
                    {
                        return;
                    }
                    client_socket = pending_client_sockets.front();
                    pending_client_sockets.pop();
                    active_client_sockets[worker_index] = client_socket;
                }
                try
                {
                    handle_client(client_socket, store, idle_timeout_seconds, role, replica_socket,
                                  replication_mutex, replica_failed, replica_session_used);
                }
                catch (const std::exception &error)
                {
                    std::cerr << "Client handler failed: " << error.what() << "\n";
                }
                catch (...)
                {
                    std::cerr << "Client handler failed.\n";
                }

                {
                    std::lock_guard<std::mutex> lock(connection_state_mutex);
                    active_client_sockets[worker_index] = -1;
                }
                ::close(client_socket);
            }
        };

        // Workers inherit this mask, leaving shutdown signals for main.
        sigset_t shutdown_signals;
        sigset_t previous_mask;
        sigemptyset(&shutdown_signals);
        sigaddset(&shutdown_signals, SIGINT);
        sigaddset(&shutdown_signals, SIGTERM);
        const int mask_error = ::pthread_sigmask(
            SIG_BLOCK, &shutdown_signals, &previous_mask);
        if (mask_error != 0)
        {
            std::cerr << "Could not block shutdown signals: " << mask_error << '\n';
            if (replica_socket != -1)
                ::close(replica_socket);
            ::close(listening_socket);
            return EXIT_FAILURE;
        }
        try
        {
            for (std::size_t i = 0; i < worker_count; ++i)
            {
                workers.emplace_back(worker_task, i);
            }
        }
        catch (...)
        {
            std::cerr << "Could not create all worker threads.\n";
            exit_status = EXIT_FAILURE;
        }

        // Restore main's mask even if creating a worker failed.
        const int restore_error = ::pthread_sigmask(
            SIG_SETMASK, &previous_mask, nullptr);

        if (restore_error != 0)
        {
            std::cerr << "Could not restore shutdown signals: " << restore_error << '\n';
            exit_status = EXIT_FAILURE;
        }

        if (exit_status != EXIT_SUCCESS)
        {
            {
                std::lock_guard<std::mutex> lock(connection_state_mutex);
                workers_stopping = true;
            }
            queue_condition.notify_all();

            for (std::thread &worker : workers)
            {
                worker.join();
            }

            ::close(listening_socket);
            if (replica_socket != -1)
                ::close(replica_socket);
            return EXIT_FAILURE;
        }

        while (!shutdown_requested)
        {
            const int client_socket = ::accept(listening_socket, nullptr, nullptr);

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
                exit_status = EXIT_FAILURE;
                break;
            }

            try
            {
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(connection_state_mutex);
                    if (pending_client_sockets.size() < pending_connection_capacity)
                    {
                        pending_client_sockets.emplace(client_socket);
                        queued = true;
                    }
                }
                if (queued)
                {
                    queue_condition.notify_one();
                }
                else
                {
                    ::close(client_socket);
                    std::cerr << "Connection rejected: waiting queue is full.\n";
                }
            }
            catch (...)
            {
                ::close(client_socket);
                std::cerr << "Could not queue client connection.\n";
                exit_status = EXIT_FAILURE;
                break;
            }
        }

        ::close(listening_socket);
        // Interrupt an ACK wait before joining; workers never close this socket.
        if (replica_socket != -1)
            ::shutdown(replica_socket, SHUT_RDWR);
        {
            std::lock_guard<std::mutex> lock(connection_state_mutex);
            workers_stopping = true;

            while (!pending_client_sockets.empty())
            {
                ::close(pending_client_sockets.front());
                pending_client_sockets.pop();
            }

            for (const int client_socket : active_client_sockets)
            {
                if (client_socket != -1)
                {
                    ::shutdown(client_socket, SHUT_RDWR);
                }
            }
        }
        queue_condition.notify_all();

        for (std::thread &worker : workers)
        {
            worker.join();
        }

        if (replica_socket != -1)
            ::close(replica_socket);
        std::cout << "Server shut down.\n";
        return exit_status;
    }
} // namespace

int main(int argc, char *argv[])
{
    // Keep the existing positional arguments; the optional ID must come last.
    std::string new_history_id;
    if (argc >= 3 && std::string(argv[argc - 2]) == "--history-id")
    {
        new_history_id = argv[argc - 1];
        if (new_history_id.empty())
        {
            std::cerr << "History ID must not be empty.\n";
            return EXIT_FAILURE;
        }
        argc -= 2;
    }
    if (argc > 6)
    {
        std::cerr << "Usage: kv_server [persistence-file] [port] [idle-timeout-seconds] [standalone|primary|replica] [replica-port] [--history-id id]\n";
        return EXIT_FAILURE;
    }

    const std::string file_path =
        argc > 1 ? argv[1] : "kv_server.log";

    std::uint16_t port = default_server_port;
    unsigned int idle_timeout_seconds = default_idle_timeout_seconds;
    const std::string role = argc >= 5 ? argv[4] : "standalone";
    std::uint16_t replica_port = default_replica_port;
    if (role != "standalone" && role != "primary" && role != "replica")
    {
        std::cerr << "Invalid role: expected standalone, primary, or replica.\n";
        return EXIT_FAILURE;
    }
    if (argc == 6 && (role != "primary" || !parse_port(argv[5], replica_port)))
    {
        std::cerr << "Replica port requires primary role and a port from 1 to 65535.\n";
        return EXIT_FAILURE;
    }

    if (argc >= 3 && !parse_port(argv[2], port))
    {
        std::cerr << "Invalid port: expected an integer from 1 to 65535.\n";
        return EXIT_FAILURE;
    }

    if (argc >= 4 && !parse_idle_timeout(argv[3], idle_timeout_seconds))
    {
        std::cerr << "Invalid idle timeout: expected an integer from 1 to "
                  << max_idle_timeout_seconds << " seconds.\n";
        return EXIT_FAILURE;
    }

    if (role == "primary" && port == replica_port)
    {
        std::cerr << "Primary and replica must listen on different ports.\n";
        return EXIT_FAILURE;
    }

    if (!install_signal_handlers())
    {
        std::perror("sigaction");
        return EXIT_FAILURE;
    }

    try
    {

        return run_server(file_path, port, idle_timeout_seconds, role, replica_port, new_history_id);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Server startup failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
