#include "key_value_store.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <iostream>
#include <filesystem>

// Helper functions
namespace
{
    void validate_field(const std::string &field, const std::string &name)
    {
        if (field.empty())
        {
            throw std::invalid_argument(name + " must not be empty");
        }
        if (field.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument(name + " must not contain a newline");
    }

    bool read_quoted(std::istringstream &input, std::string &result)
    {
        input >> std::ws;

        if (input.peek() != '"')
            return false;

        return static_cast<bool>(input >> std::quoted(result));
    }

    bool reached_end(std::istringstream &input)
    {
        input >> std::ws;
        return input.eof();
    }
}

// Construtor
KeyValueStore::KeyValueStore(const std::string &file_path)
    : file_path_(file_path)
{
    replay();
    open_log();
}

// Class Methods
void KeyValueStore::set(
    const std::string &key,
    const std::string &value)
{
    validate_field(key, "Key");
    validate_field(value, "Value");

    std::ostringstream record;
    record << "SET " << std::quoted(key) << ' ' << std::quoted(value);

    std::lock_guard<std::mutex> lock(mutex_);

    append_record(record.str());

    data_[key] = value;
}

std::optional<std::string> KeyValueStore::get(
    const std::string &key) const
{
    std::lock_guard<std::mutex> lock(mutex_);

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
    validate_field(key, "Key");

    std::lock_guard<std::mutex> lock(mutex_);

    if (!data_.contains(key))
    {
        return false;
    }

    std::ostringstream record;
    record << "DELETE " << std::quoted(key);

    append_record(record.str());

    return data_.erase(key) > 0;
}

void KeyValueStore::open_log()
{
    log_file_.clear();
    log_file_.open(file_path_, std::ios::app);
    if (!log_file_.is_open())
        throw std::runtime_error("Could not open persistence file");
}

void KeyValueStore::append_record(const std::string &record)
{
    if (file_path_.empty())
        return;

    if (!log_file_.is_open())
        throw std::runtime_error("Persistence file is not open");

    log_file_ << record << '\n';

    if (!log_file_)
        throw std::runtime_error("Could not write persistence record");

    log_file_.flush();

    if (!log_file_)
        throw std::runtime_error("Could not flush persistence record");
}

std::size_t
KeyValueStore::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.size();
}

bool KeyValueStore::contains(
    const std::string &key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.contains(key);
}

void KeyValueStore::compact()
{
    if (file_path_.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::filesystem::path temporary_path = file_path_;
    temporary_path += ".tmp";
    std::ofstream tmp_file(temporary_path, std::ios::trunc);

    if (!tmp_file.is_open())
        throw std::runtime_error("Could not open compacted log");

    for (const auto &[key, value] : data_)
    {
        tmp_file << "SET "
                 << std::quoted(key)
                 << ' '
                 << std::quoted(value)
                 << '\n';
    }

    if (!tmp_file)
        throw std::runtime_error("Could not write compacted log");

    tmp_file.flush();

    if (!tmp_file)
        throw std::runtime_error("Could not flush compacted log");

    tmp_file.close();
    if (!tmp_file)
    {
        throw std::runtime_error("Could not close compacted log");
    }

    // Close the old stream before replacement; otherwise it could keep writing
    // to the old file even after rename changes which file the path names.
    log_file_.close();
    if (!log_file_)
        throw std::runtime_error("Could not close persistence file before compaction");

    try
    {
        std::filesystem::rename(temporary_path, file_path_);
    }
    catch (...)
    {
        // Replacement failed: the original log is still the active file.
        open_log();
        throw;
    }
    open_log();
}

// Replay method, runs at startup and sends all data in-memory
void KeyValueStore::replay()
{
    std::ifstream log_file(file_path_);

    if (!log_file.is_open())
    {
        std::ofstream new_log_file(file_path_, std::ios::app);
        if (!new_log_file.is_open())
        {
            throw std::runtime_error(
                "Could not create persistence file: " + file_path_);
        }
        return;
    }

    std::size_t line_number = 0;
    std::string record;

    while (std::getline(log_file, record))
    {
        ++line_number;
        if (log_file.eof())
        {
            std::cerr << "Warning: ignoring incomplete final record at line "
                      << line_number << '\n';
            break;
        }

        if (record.empty())
            continue;

        // parse this record and apply it directly to data_.
        std::string operation;
        std::string key;
        std::string value;

        std::istringstream input(record);

        input >> operation;

        if (operation == "SET")
        {
            if (!read_quoted(input, key) ||
                !read_quoted(input, value) ||
                !reached_end(input) ||
                key.empty() ||
                value.empty())
            {
                throw std::runtime_error(
                    "Malformed SET at line " + std::to_string(line_number));
            }
            data_[key] = value;
        }
        else if (operation == "DELETE")
        {
            if (!read_quoted(input, key) ||
                !reached_end(input) ||
                key.empty())
            {
                throw std::runtime_error(
                    "Malformed DELETE at line " + std::to_string(line_number));
            }
            data_.erase(key);
        }
        else
        {
            throw std::runtime_error("Unknown operation: " + operation);
        }
    }

    if (log_file.bad())
        throw std::runtime_error("Could not read persistence file");
}
