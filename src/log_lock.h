#pragma once

#include <string>

// A process-level lock held for the owner's lifetime. Unlike std::mutex, this
// coordinates separate server/snapshot processes using the same configured log.
class LogLock
{
public:
    explicit LogLock(const std::string &log_path);
    LogLock(const LogLock &) = delete;
    LogLock &operator=(const LogLock &) = delete;
    ~LogLock();

private:
    int descriptor_ = -1;
};
