#include "log_lock.h"

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

LogLock::LogLock(const std::string &log_path)
{
    if (log_path.empty())
        throw std::runtime_error("Log path must not be empty");
    // A separate lock file survives compaction replacing the log's inode.
    const std::string lock_path =
        std::filesystem::weakly_canonical(std::filesystem::absolute(log_path)).string() + ".lock";
    descriptor_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (descriptor_ == -1)
        throw std::runtime_error("Could not open log lock: " + lock_path + ": " + std::strerror(errno));
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) == -1)
    {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error("Log is in use or cannot be locked: " + log_path + ": " + std::strerror(error));
    }
}

LogLock::~LogLock()
{
    if (descriptor_ != -1)
        ::close(descriptor_); // Closing releases the lock, including on exceptions.
}
