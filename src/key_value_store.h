#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

class KeyValueStore
{
public:
    KeyValueStore() = default;
    explicit KeyValueStore(const std::string &file_path);

    void set(const std::string &key, const std::string &value);
    std::optional<std::string> get(const std::string &key) const;
    bool remove(const std::string &key);
    std::size_t size() const;
    bool contains(const std::string &key) const;

private:
    void replay();
    void append_record(const std::string &record);
    std::string file_path_;
    std::unordered_map<std::string, std::string> data_;
};
