#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <exception>
#include <latch>
#include <thread>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t key_count = 100;
    constexpr std::size_t warmup_count = 100;
    constexpr std::size_t request_count = 100000;
    constexpr int timeout_seconds = 5;
    using Clock = std::chrono::steady_clock;

    void fail(const std::string &operation)
    {
        throw std::runtime_error(operation + ": " + std::strerror(errno));
    }

    void connect_to_server(int socket, unsigned short port)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // Nonblocking connect lets poll impose a finite connection timeout.
        const int flags = ::fcntl(socket, F_GETFL, 0);
        if (flags == -1 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1)
            fail("fcntl");
        if (::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == -1)
        {
            if (errno != EINPROGRESS)
                fail("connect");
            pollfd pending{socket, POLLOUT, 0};
            const int ready = ::poll(&pending, 1, timeout_seconds * 1000);
            if (ready == -1)
                fail("poll connect");
            if (ready == 0)
                throw std::runtime_error("Connection timed out");
            int error = 0;
            socklen_t length = sizeof(error);
            if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == -1)
                fail("getsockopt");
            if (error != 0)
                throw std::runtime_error(std::string("connect: ") + std::strerror(error));
        }
        if (::fcntl(socket, F_SETFL, flags) == -1)
            fail("restore blocking socket");

        timeval timeout{};
        timeout.tv_sec = timeout_seconds;
        if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
            ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1)
            fail("setsockopt timeout");
    }

    void send_all(int socket, const std::string &request)
    {
        std::size_t sent = 0;
        while (sent < request.size())
        {
            const ssize_t count = ::send(socket, request.data() + sent, request.size() - sent, 0);
            if (count == -1)
            {
                if (errno == EINTR)
                    continue;
                fail("send");
            }
            if (count == 0)
                throw std::runtime_error("Connection closed while sending");
            sent += static_cast<std::size_t>(count);
        }
    }

    std::string receive_line(int socket, std::string &pending)
    {
        while (true)
        {
            const auto newline = pending.find('\n');
            if (newline != std::string::npos)
            {
                std::string response = pending.substr(0, newline + 1);
                pending.erase(0, newline + 1);
                return response;
            }
            // Our fixed values produce tiny replies; unexpected input must stay bounded.
            if (pending.size() > 4096)
                throw std::runtime_error("Response exceeds benchmark limit");
            char buffer[1024];
            const ssize_t count = ::recv(socket, buffer, sizeof(buffer), 0);
            if (count == -1)
            {
                if (errno == EINTR)
                    continue;
                fail("recv");
            }
            if (count == 0)
                throw std::runtime_error("Server disconnected before a complete response");
            pending.append(buffer, static_cast<std::size_t>(count));
        }
    }

    // Read-only check: also usable after restarting the server with the SET log.
    void verify_set_values(int socket, std::string &pending)
    {
        for (std::size_t i = 0; i < key_count; ++i)
        {
            const std::string key = "benchmark-key-" + std::to_string(i);
            const std::string value = "benchmark-write-" + std::to_string(i);
            send_all(socket, "GET \"" + key + "\"\n");
            if (receive_line(socket, pending) != "VALUE \"" + value + "\"\n")
                throw std::runtime_error("Final SET value mismatch for " + key);
        }
    }

    void prepare_requests(int socket, const std::string &workload,
                          std::vector<std::string> &requests,
                          std::vector<std::string> &expected)
    {
        std::string pending;
        for (std::size_t i = 0; i < key_count; ++i)
        {
            const std::string key = "benchmark-key-" + std::to_string(i);
            const std::string value = "benchmark-value-" + std::to_string(i);
            send_all(socket, "SET \"" + key + "\" \"" + value + "\"\n");
            if (receive_line(socket, pending) != "OK\n")
                throw std::runtime_error("Key preparation failed");
            if (workload == "SET")
            {
                const std::string updated_value = "benchmark-write-" + std::to_string(i);
                requests.push_back("SET \"" + key + "\" \"" + updated_value + "\"\n");
                expected.push_back("OK\n");
            }
            else
            {
                requests.push_back("GET \"" + key + "\"\n");
                expected.push_back("VALUE \"" + value + "\"\n");
            }
        }
    }

    void benchmark(const std::vector<int> &sockets, const std::string &workload)
    {
        const std::size_t count = sockets.size();
        const std::size_t keys_per_client = key_count / count;
        const std::size_t requests_per_client = request_count / count;
        std::vector<std::string> requests;
        std::vector<std::string> expected;
        prepare_requests(sockets[0], workload, requests, expected);

        for (std::size_t client = 0; client < count; ++client)
        {
            std::string pending;
            for (std::size_t i = 0; i < warmup_count / count; ++i)
            {
                const std::size_t index = client * keys_per_client + i % keys_per_client;
                send_all(sockets[client], requests[index]);
                if (receive_line(sockets[client], pending) != expected[index])
                    throw std::runtime_error("Unexpected response during warm-up");
            }
        }

        // Allocate before timing. Each thread writes only to its own sample range.
        std::vector<double> latencies(request_count);
        std::array<Clock::time_point, 2> finished{};
        std::array<std::exception_ptr, 2> errors{};
        std::latch ready(count);
        std::latch start_gate(1);
        std::vector<std::thread> threads;
        threads.reserve(count);
        try
        {
            for (std::size_t client = 0; client < count; ++client)
            {
                // Capture the client number by value; it identifies this thread's range.
                threads.emplace_back([&, client]()
                {
                    std::string pending;
                    ready.count_down();
                    start_gate.wait();
                    try
                    {
                        for (std::size_t i = 0; i < requests_per_client; ++i)
                        {
                            const std::size_t index = client * keys_per_client + i % keys_per_client;
                            const auto request_start = Clock::now();
                            send_all(sockets[client], requests[index]);
                            const std::string response = receive_line(sockets[client], pending);
                            if (response != expected[index])
                                throw std::runtime_error("Unexpected response: " + response);
                            const auto request_end = Clock::now();
                            latencies[client * requests_per_client + i] =
                                std::chrono::duration<double, std::micro>(request_end - request_start).count();
                            finished[client] = request_end;
                        }
                    }
                    catch (...)
                    {
                        // A thread cannot throw into main. Save its error for after joining.
                        errors[client] = std::current_exception();
                    }
                });
            }
        }
        catch (...)
        {
            // A failed thread creation must not leave earlier threads waiting or unjoined.
            start_gate.count_down();
            for (std::thread &thread : threads)
                thread.join();
            throw;
        }
        ready.wait();
        const auto start = Clock::now();
        start_gate.count_down();
        for (std::thread &thread : threads)
            thread.join();

        auto last_finished = start;
        for (std::size_t client = 0; client < count; ++client)
        {
            if (errors[client])
                std::rethrow_exception(errors[client]);
            last_finished = std::max(last_finished, finished[client]);
        }
        const double seconds = std::chrono::duration<double>(last_finished - start).count();
        if (workload == "SET")
        {
            std::string pending;
            verify_set_values(sockets[0], pending);
        }
        std::sort(latencies.begin(), latencies.end());
        const double median = (latencies[request_count / 2 - 1] + latencies[request_count / 2]) / 2;
        const std::size_t p95_rank = (95 * request_count + 99) / 100;
        std::cout << std::fixed << std::setprecision(3)
                  << "Workload: " << workload << ", " << count << " client(s), " << key_count
                  << " total keys, " << warmup_count << " total warm-up requests\n"
                  << "Completed requests: " << request_count << '\n'
                  << "Total duration (s): " << seconds << '\n'
                  << "Throughput (requests/s): " << request_count / seconds << '\n'
                  << "Median latency (us): " << median << '\n'
                  << "p95 latency (us, nearest rank): " << latencies[p95_rank - 1] << '\n';
    }

}

int main(int argc, char *argv[])
{
    // main owns all sockets and closes them after every client thread is joined.
    std::vector<int> sockets;
    try
    {
        if (argc > 4)
            throw std::runtime_error("Usage: kv_benchmark [port] [GET|SET|VERIFY] [1|2]");
        const std::string text = argc >= 2 ? argv[1] : "6380";
        const std::string workload = argc >= 3 ? argv[2] : "GET";
        const std::string count_text = argc == 4 ? argv[3] : "1";
        if (count_text != "1" && count_text != "2")
            throw std::runtime_error("Client count must be 1 or 2");
        const std::size_t client_count = count_text == "2" ? 2 : 1;
        if (workload != "GET" && workload != "SET" && workload != "VERIFY")
            throw std::runtime_error("Workload must be GET, SET, or VERIFY");
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Port must be an integer from 1 to 65535");
        const unsigned long port = std::stoul(text);
        if (port == 0 || port > 65535)
            throw std::runtime_error("Port must be an integer from 1 to 65535");
        if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            fail("signal");
        // VERIFY needs only one connection, even for a two-client SET log.
        sockets.resize(workload == "VERIFY" ? 1 : client_count, -1);
        for (int &socket : sockets)
        {
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == -1)
                fail("socket");
            connect_to_server(socket, static_cast<unsigned short>(port));
        }
        if (workload == "VERIFY")
        {
            std::string pending;
            verify_set_values(sockets[0], pending);
            std::cout << "Verified final SET values for " << key_count << " keys (read-only).\n";
        }
        else
            benchmark(sockets, workload);
        for (int &socket : sockets)
            ::close(socket);
        return 0;
    }
    catch (const std::exception &error)
    {
        for (int &socket : sockets)
            if (socket != -1)
                ::close(socket);
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
