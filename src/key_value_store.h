#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

class KeyValueStore
{
public:
    KeyValueStore() = default;
    // Unpartitioned standalone keeps its original format; replication numbers records.
    // A partition adds a fixed routing header and requires matching startup configuration.
    // Supply an ID when creating a numbered log; reopen without one to recover it.
    explicit KeyValueStore(const std::string &file_path, bool numbered_history = false,
                           const std::string &new_history_id = "", int partition_id = -1);

    // Partition identity is fixed at construction; -1 means unpartitioned.
    bool owns_key(const std::string &key) const;
    void set(const std::string &key, const std::string &value);
    std::optional<std::string> get(const std::string &key) const;
    bool remove(const std::string &key);
    std::size_t size() const;
    bool contains(const std::string &key) const;
    void compact();
    std::uint64_t last_sequence() const;
    std::string history_id() const;
    // Copy once for startup recovery; no store lock is held during network I/O.
    std::vector<std::string> history_records() const;
    // Verify order/resends, persist and apply a peer mutation; return its ACK number.
    std::uint64_t apply_replica_record(const std::string &record);

private:
    void require_owned_key(const std::string &key) const;
    void replay();
    void append_record(const std::string &record);
    void open_log();
    std::string file_path_;
    std::ofstream log_file_;
    std::unordered_map<std::string, std::string> data_;
    int partition_id_ = -1;
    bool partition_header_present_ = false;
    bool numbered_history_ = false;
    std::string history_id_;
    bool write_failed_ = false;
    // Retain records to verify old resends without applying them again.
    std::vector<std::string> history_;
    mutable std::mutex mutex_;
};
