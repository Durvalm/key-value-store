#include "key_value_store.h"

void KeyValueStore::set(
    const std::string &key,
    const std::string &value)
{
    data_[key] = value;
}

std::optional<std::string> KeyValueStore::get(
    const std::string &key) const
{
    auto entry = data_.find(key);
    if (entry == data_.end())
    {
        return std::nullopt;
    }
    return entry->second;
}

bool KeyValueStore::remove(
    const std::string &key)
{
    return data_.erase(key) > 0;
}

std::size_t
KeyValueStore::size() const
{
    return data_.size();
}
