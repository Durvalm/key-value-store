#include "key_value_store.h"
#include "partition.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <charconv>
#include <algorithm>

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

    bool valid_history_id(const std::string &id)
    {
        return !id.empty() && id.size() <= 128 &&
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
    }

    std::uint64_t read_sequence(std::istringstream &input)
    {
        std::string text;
        input >> text;
        std::uint64_t sequence = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), sequence);
        if (error != std::errc{} || end != text.data() + text.size() || sequence == 0)
            throw std::runtime_error("Expected a positive sequence number in numbered history");
        return sequence;
    }

    struct Mutation
    {
        std::string operation;
        std::string key;
        std::string value;
    };

    Mutation read_mutation(std::istringstream &input)
    {
        Mutation mutation;
        input >> mutation.operation;
        if ((mutation.operation != "SET" && mutation.operation != "DELETE") ||
            !read_quoted(input, mutation.key) || mutation.key.empty() ||
            (mutation.operation == "SET" &&
             (!read_quoted(input, mutation.value) || mutation.value.empty())) ||
            !reached_end(input))
            throw std::runtime_error("Malformed mutation record");
        validate_field(mutation.key, "Key");
        if (mutation.operation == "SET")
            validate_field(mutation.value, "Value");
        return mutation;
    }

    std::string mutation_command(const Mutation &mutation)
    {
        std::ostringstream command;
        command << mutation.operation << ' ' << std::quoted(mutation.key);
        if (mutation.operation == "SET")
            command << ' ' << std::quoted(mutation.value);
        return command.str();
    }
}

// Construtor
KeyValueStore::KeyValueStore(const std::string &file_path, bool numbered_history,
                           const std::string &new_history_id, int partition_id)
    : file_path_(file_path), partition_id_(partition_id), numbered_history_(numbered_history)
{
    if (partition_id_ < -1 || partition_id_ > 1)
        throw std::runtime_error("Partition ID must be 0 or 1");
    if (!new_history_id.empty() && (!numbered_history_ || !valid_history_id(new_history_id)))
        throw std::runtime_error("History ID requires numbered mode and 1-128 letters, digits, dots, underscores or hyphens");

    log_lock_.emplace(file_path_);
    initialize(new_history_id);
}

KeyValueStore::KeyValueStore(const std::string &file_path, const ClusterLayout &layout,
                           const std::string &group_id, bool read_only)
    : file_path_(file_path), layout_(layout), group_id_(group_id), numbered_history_(true), read_only_(read_only)
{
    const auto &expected_id = layout_->group(group_id_).history_id;
    log_lock_.emplace(file_path_);
    if (read_only_)
    {
        replay(false);
        if (history_id_.empty() || history_id_ != expected_id)
            throw std::runtime_error("Source log requires the expected saved HISTORY header");
    }
    else
        initialize(expected_id);
}

std::string KeyValueStore::ownership_header() const
{
    return layout_ ? layout_->ownership_header(group_id_) : "";
}

std::string KeyValueStore::node_description(const std::string &role) const
{
    return layout_ ? layout_->node_description(group_id_, role) : "";
}

void KeyValueStore::initialize(const std::string &new_history_id)
{
    replay();
    const bool new_history = numbered_history_ && history_id_.empty();
    if (new_history)
    {
        if (new_history_id.empty())
            throw std::runtime_error("New numbered history requires an explicit history ID (--history-id)");
        history_id_ = new_history_id;
    }
    if (!new_history_id.empty() && history_id_ != new_history_id)
        throw std::runtime_error("Supplied history ID does not match the saved history ID");

    open_log();
    if (layout_ && !layout_header_present_ && !partition_header_present_)
        log_file_ << ownership_header() << '\n';
    if (partition_id_ != -1 && !partition_header_present_)
        log_file_ << "PARTITION " << partition_id_ << " fnv1a64-mod2-v1\n";
    if (new_history)
        log_file_ << "HISTORY " << history_id_ << '\n';
    log_file_.flush();
    if (!log_file_)
        throw std::runtime_error("Could not write log headers");
}

bool KeyValueStore::owns_key(const std::string &key) const
{
    if (layout_)
        return layout_->owner(key).id == group_id_;
    return partition_id_ == -1 || partition_for_key(key) == static_cast<std::size_t>(partition_id_);
}

void KeyValueStore::require_owned_key(const std::string &key) const
{
    if (!owns_key(key))
        throw std::runtime_error("Key belongs to another partition");
}

// Class Methods
void KeyValueStore::set(
    const std::string &key,
    const std::string &value)
{
    validate_field(key, "Key");
    if (read_only_)
        throw std::runtime_error("Source snapshot store is read-only");
    require_owned_key(key);
    validate_field(value, "Value");

    std::ostringstream record;
    record << "SET " << std::quoted(key) << ' ' << std::quoted(value);

    std::lock_guard<std::mutex> lock(mutex_);

    try
    {
        append_record(record.str());
        data_[key] = value;
    }
    catch (...)
    {
        write_failed_ = true;
        throw;
    }
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
    if (read_only_)
        throw std::runtime_error("Source snapshot store is read-only");
    require_owned_key(key);

    std::lock_guard<std::mutex> lock(mutex_);

    if (write_failed_)
        throw std::runtime_error("Writes disabled after persistence/application failure");

    if (!data_.contains(key))
    {
        return false;
    }

    std::ostringstream record;
    record << "DELETE " << std::quoted(key);

    try
    {
        append_record(record.str());
    }
    catch (...)
    {
        write_failed_ = true;
        throw;
    }

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
    if (read_only_)
        throw std::runtime_error("Source snapshot store is read-only");
    if (write_failed_)
        throw std::runtime_error("Writes disabled after persistence/application failure");

    if (file_path_.empty())
        return;

    if (!log_file_.is_open())
        throw std::runtime_error("Persistence file is not open");

    const std::string persisted = numbered_history_
        ? std::to_string(history_.size() + 1) + ' ' + record : record;
    log_file_ << persisted << '\n';

    if (!log_file_)
        throw std::runtime_error("Could not write persistence record");

    log_file_.flush();

    if (!log_file_)
        throw std::runtime_error("Could not flush persistence record");

    if (numbered_history_)
        history_.push_back(persisted);
}

std::uint64_t KeyValueStore::last_sequence() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return history_.size();
}

std::string KeyValueStore::history_id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return history_id_;
}

std::vector<std::string> KeyValueStore::history_records() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return history_;
}

std::uint64_t KeyValueStore::apply_replica_record(const std::string &record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!numbered_history_ || read_only_ || write_failed_)
        throw std::runtime_error("Numbered replica history is not writable");
    if (record.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Replication record must contain exactly one line");

    std::istringstream input(record);
    const std::uint64_t sequence = read_sequence(input);
    std::string command_text;
    std::getline(input >> std::ws, command_text);
    if (command_text.size() > 4096)
        throw std::runtime_error("Replicated command exceeds 4096 bytes");
    std::istringstream command_input(command_text);
    const Mutation mutation = read_mutation(command_input);
    require_owned_key(mutation.key);
    const std::string command = mutation_command(mutation);
    const std::string canonical = std::to_string(sequence) + ' ' + command;

    if (sequence <= history_.size())
    {
        if (history_[sequence - 1] != canonical)
            throw std::runtime_error("Conflicting replication resend");
        return sequence; // Already applied: ACK without changing data or the log.
    }
    if (sequence != history_.size() + 1)
        throw std::runtime_error("Gap in replication sequence");
    if (mutation.operation == "DELETE" && !data_.contains(mutation.key))
        throw std::runtime_error("Replicated DELETE must remove an existing key");

    try
    {
        append_record(command);
        if (mutation.operation == "SET")
            data_[mutation.key] = mutation.value;
        else
            data_.erase(mutation.key);
    }
    catch (...)
    {
        write_failed_ = true;
        throw;
    }
    return sequence;
}

std::vector<std::pair<std::string, std::string>> KeyValueStore::snapshot() const
{
    std::vector<std::pair<std::string, std::string>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (write_failed_)
            throw std::runtime_error("Cannot snapshot a store after a write failure");
        entries.assign(data_.begin(), data_.end());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
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
    if (numbered_history_)
        throw std::runtime_error("Cannot compact numbered replication history");
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

    if (partition_id_ != -1)
        tmp_file << "PARTITION " << partition_id_ << " fnv1a64-mod2-v1\n";

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
void KeyValueStore::replay(bool create_missing)
{
    std::ifstream log_file(file_path_);

    if (!log_file.is_open())
    {
        if (!create_missing)
            throw std::runtime_error("Could not read existing source log: " + file_path_);
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

    // New layout-managed logs persist the complete ownership definition.
    if (log_file.peek() == 'O')
    {
        std::getline(log_file, record);
        ++line_number;
        if (!log_file || log_file.eof() || !layout_ || record != ownership_header())
            throw std::runtime_error("Saved ownership layout does not match startup configuration");
        layout_header_present_ = true;
        if (log_file.peek() == std::ifstream::traits_type::eof())
            throw std::runtime_error("Layout log is missing its HISTORY header");
    }
    // Existing 7A partition logs may be reopened only with the equivalent
    // two-group modulo layout and matching partition index/history ID.
    int expected_partition = partition_id_;
    if (layout_ && layout_->routing() == "fnv1a64-mod-v1" && layout_->groups().size() == 2)
        expected_partition = layout_->groups()[0].id == group_id_ ? 0 : 1;

    // Partition metadata precedes HISTORY (if present) and is not a mutation.
    if (layout_header_present_ && log_file.peek() == 'P')
        throw std::runtime_error("Unexpected PARTITION header after OWNERSHIP");
    if (log_file.peek() == 'P')
    {
        std::getline(log_file, record);
        ++line_number;
        std::istringstream header(record);
        std::string marker, saved_partition, routing;
        if (!log_file || log_file.eof() ||
            !(header >> marker >> saved_partition >> routing) || !reached_end(header) ||
            marker != "PARTITION" || expected_partition == -1 ||
            saved_partition != std::to_string(expected_partition) || routing != "fnv1a64-mod2-v1")
            throw std::runtime_error("Partition log/configuration mismatch or malformed PARTITION header");
        partition_header_present_ = true;
        if (numbered_history_ && log_file.peek() == std::ifstream::traits_type::eof())
            throw std::runtime_error("Partitioned numbered log is missing its HISTORY header");
    }
    else if (((layout_ && !layout_header_present_) || partition_id_ != -1) &&
             log_file.peek() != std::ifstream::traits_type::eof())
        throw std::runtime_error("Existing log has no PARTITION header; use a fresh partition log");

    if (numbered_history_ && log_file.peek() != std::ifstream::traits_type::eof())
    {
        std::getline(log_file, record);
        ++line_number;
        if (!log_file || log_file.eof())
            throw std::runtime_error("Incomplete history header");
        std::istringstream header(record);
        std::string marker;
        header >> marker >> history_id_;
        if (marker != "HISTORY" || !valid_history_id(history_id_) || !reached_end(header))
            throw std::runtime_error("Missing or malformed HISTORY header; use a fresh numbered log");
    }

    while (std::getline(log_file, record))
    {
        ++line_number;
        if (log_file.eof())
        {
            if (numbered_history_)
                throw std::runtime_error("Incomplete numbered history tail; repair before restarting");
            std::cerr << "Warning: ignoring incomplete final record at line "
                      << line_number << '\n';
            break;
        }

        if (record.empty() && !numbered_history_)
            continue;

        std::istringstream input(record);
        try
        {
            if (numbered_history_ && read_sequence(input) != history_.size() + 1)
                throw std::runtime_error("Nonconsecutive numbered history");
            const Mutation mutation = read_mutation(input);
            require_owned_key(mutation.key);
            if (mutation.operation == "SET")
                data_[mutation.key] = mutation.value;
            else
            {
                if (numbered_history_ && !data_.contains(mutation.key))
                    throw std::runtime_error("Numbered DELETE has no matching key");
                data_.erase(mutation.key);
            }
            if (numbered_history_)
                history_.push_back(std::to_string(history_.size() + 1) + ' ' + mutation_command(mutation));
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error("Invalid history at line " + std::to_string(line_number) + ": " + error.what());
        }
    }

    if (log_file.bad())
        throw std::runtime_error("Could not read persistence file");
}
