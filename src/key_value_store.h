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
    // Standalone keeps its original format; replication uses numbered records.
    explicit KeyValueStore(const std::string &file_path, bool numbered_history = false);

    void set(const std::string &key, const std::string &value);
    std::optional<std::string> get(const std::string &key) const;
    bool remove(const std::string &key);
    std::size_t size() const;
    bool contains(const std::string &key) const;
    void compact();
    std::uint64_t last_sequence() const;
    // Verify order/resends, persist and apply a peer mutation; return its ACK number.
    std::uint64_t apply_replica_record(const std::string &record);

private:
    void replay();
    void append_record(const std::string &record);
    void open_log();
    std::string file_path_;
    std::ofstream log_file_;
    std::unordered_map<std::string, std::string> data_;
    bool numbered_history_ = false;
    bool write_failed_ = false;
    // Retain records to verify old resends without applying them again.
    std::vector<std::string> history_;
    mutable std::mutex mutex_;
};
