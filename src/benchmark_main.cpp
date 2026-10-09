#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
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

    void benchmark(int socket, const std::string &workload)
    {
        std::string pending;
        std::vector<std::string> requests;
        std::vector<std::string> expected;
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

        for (std::size_t i = 0; i < warmup_count; ++i)
        {
            const std::size_t index = i % key_count;
            send_all(socket, requests[index]);
            if (receive_line(socket, pending) != expected[index])
                throw std::runtime_error("Unexpected " + workload + " response during warm-up");
        }

        std::vector<double> latencies;
        latencies.reserve(request_count);
        const auto start = Clock::now();
        for (std::size_t i = 0; i < request_count; ++i)
        {
            const std::size_t index = i % key_count;
            const auto request_start = Clock::now();
            send_all(socket, requests[index]);
            const std::string response = receive_line(socket, pending);
            if (response != expected[index])
                throw std::runtime_error("Unexpected " + workload + " response: " + response);
            const auto request_end = Clock::now();
            const double microseconds =
                std::chrono::duration<double, std::micro>(request_end - request_start).count();
            latencies.push_back(microseconds);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        // Validation is outside timing, and must pass before reporting success.
        if (workload == "SET")
            verify_set_values(socket, pending);
        std::sort(latencies.begin(), latencies.end());
        const double median = (latencies[request_count / 2 - 1] + latencies[request_count / 2]) / 2;
        const std::size_t p95_rank = (95 * request_count + 99) / 100;

        std::cout << std::fixed << std::setprecision(3)
                  << "Workload: " << workload << ", 1 client, " << key_count << " keys, " << warmup_count << " warm-up requests\n"
                  << "Completed requests: " << request_count << '\n'
                  << "Total duration (s): " << seconds << '\n'
                  << "Throughput (requests/s): " << request_count / seconds << '\n'
                  << "Median latency (us): " << median << '\n'
                  << "p95 latency (us, nearest rank): " << latencies[p95_rank - 1] << '\n';
    }
}

int main(int argc, char *argv[])
{
    // main owns the socket and closes it on both success and failure.
    int client_socket = -1;
    try
    {
        if (argc > 3)
            throw std::runtime_error("Usage: kv_benchmark [port] [GET|SET|VERIFY]");
        const std::string text = argc >= 2 ? argv[1] : "6380";
        const std::string workload = argc == 3 ? argv[2] : "GET";
        if (workload != "GET" && workload != "SET" && workload != "VERIFY")
            throw std::runtime_error("Workload must be GET, SET, or VERIFY");
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Port must be an integer from 1 to 65535");
        const unsigned long port = std::stoul(text);
        if (port == 0 || port > 65535)
            throw std::runtime_error("Port must be an integer from 1 to 65535");
        if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            fail("signal");
        client_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client_socket == -1)
            fail("socket");
        connect_to_server(client_socket, static_cast<unsigned short>(port));
        if (workload == "VERIFY")
        {
            std::string pending;
            verify_set_values(client_socket, pending);
            std::cout << "Verified final SET values for " << key_count << " keys (read-only).\n";
        }
        else
            benchmark(client_socket, workload);
        ::close(client_socket);
        return 0;
    }
    catch (const std::exception &error)
    {
        if (client_socket != -1)
            ::close(client_socket);
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
