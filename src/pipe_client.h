#pragma once

#include <string>
#include <vector>
#include <optional>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace bldr {

// PipeClient connects to a Unix domain socket (or Windows named pipe)
// and provides simple read/write operations for raw bytes.
class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();

    // Non-copyable, non-movable
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    PipeClient(PipeClient&&) = delete;
    PipeClient& operator=(PipeClient&&) = delete;

    // Connect to the pipe socket at the given path.
    // Returns true on success, false on failure.
    bool connect(const std::string& pipe_path);

    // Close the connection.
    void close();

    // Check if connected.
    bool is_connected() const;

    // Read data from the pipe.
    // Returns empty vector if connection is closed or error occurs.
    std::vector<uint8_t> read();

    // Read with timeout (milliseconds).
    // Returns empty vector if timeout or error.
    std::vector<uint8_t> read_with_timeout(int timeout_ms);

    // Write data to the pipe.
    // Returns true on success, false on failure.
    bool write(const uint8_t* data, size_t length);

    // Write data to the pipe.
    bool write(const std::vector<uint8_t>& data);

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    std::atomic<bool> connected_{false};
    std::mutex mutex_;
};

} // namespace bldr
