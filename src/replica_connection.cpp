#include "replica_connection.h"
#include "line_connection.h"

#include <unistd.h>

#include <charconv>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

int connect_to_replica(std::uint16_t port, const std::string &history_id,
                       const std::vector<std::string> &history, const std::string &ownership)
{
    const int replica_socket = connect_loopback(port);
    try
    {
        std::ostringstream handshake;
        handshake << "REPLICA " << history_id;
        if (!ownership.empty())
            handshake << ' ' << std::quoted(ownership);
        handshake << '\n';
        const std::string response = exchange_message(replica_socket, handshake.str());
        std::istringstream reply(response);
        std::string marker, replica_id, sequence_text, extra;
        if (!(reply >> marker >> replica_id >> sequence_text) || (reply >> extra) ||
            marker != "READY" || replica_id != history_id)
            throw std::runtime_error("Unexpected replica handshake: " + response);

        std::uint64_t replica_sequence = 0;
        const auto [end, error] = std::from_chars(
            sequence_text.data(), sequence_text.data() + sequence_text.size(), replica_sequence);
        if (error != std::errc{} || end != sequence_text.data() + sequence_text.size())
            throw std::runtime_error("Invalid replica progress: " + sequence_text);
        if (replica_sequence > history.size())
            throw std::runtime_error("Replica is ahead of primary; refusing to overwrite history");

        std::cout << "Replica reports sequence " << replica_sequence
                  << "; verifying " << replica_sequence << " records and catching up "
                  << history.size() - replica_sequence << " records.\n";
        // Resends verify the existing prefix; only missing records are applied.
        // Client workers have not started, so new writes cannot overtake recovery.
        for (std::size_t i = 0; i < history.size(); ++i)
            send_to_replica(replica_socket, history[i], "ACK " + std::to_string(i + 1) + '\n');
        std::cout << "Replica ready at sequence " << history.size() << '\n';
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
    const std::string response = exchange_message(replica_socket, line + '\n');
    if (response != expected_response)
        throw std::runtime_error("Unexpected replica acknowledgment: " + response);
}
