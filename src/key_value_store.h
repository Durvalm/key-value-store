#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

class KeyValueStore
{
public:
    void set(const std::string &key, const std::string &value);
    std::optional<std::string> get(const std::string &key) const;
    bool remove(const std::string &key);
    std::size_t size() const;

private:
    std::unordered_map<std::string, std::string> data_;
};