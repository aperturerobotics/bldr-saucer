#include "pipe_client.h"
#include <cstring>
#include <iostream>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#endif

namespace bldr {

PipeClient::~PipeClient() {
    close();
}

bool PipeClient::connect(const std::string& pipe_path) {
    std::lock_guard<std::mutex> rlock(read_mtx_);
    std::lock_guard<std::mutex> wlock(write_mtx_);

#ifdef _WIN32
    // Windows named pipe connection
    std::string pipe_name = "\\\\.\\pipe\\" + pipe_path;
    handle_ = CreateFileA(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (handle_ == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to connect to pipe: " << GetLastError() << std::endl;
        return false;
    }

    // Set pipe to byte read mode.
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
        std::cerr << "Failed to set pipe mode: " << GetLastError() << std::endl;
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return false;
    }
#else
    // Unix domain socket connection
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    // pipe_path might be relative, use it as-is
    if (pipe_path.length() >= sizeof(addr.sun_path)) {
        std::cerr << "Pipe path too long: " << pipe_path << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, pipe_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to pipe: " << strerror(errno) << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
#endif

    connected_ = true;
    return true;
}

void PipeClient::close() {
    // Set disconnected first so blocked reads/writes return.
    connected_ = false;

#ifndef _WIN32
    // Shut down the socket to unblock any thread in a blocking read.
    // This is safe to call without holding locks since shutdown on a
    // valid fd is thread-safe and causes blocked read/write to return.
    int fd = fd_;
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
    }
#endif

    std::lock_guard<std::mutex> rlock(read_mtx_);
    std::lock_guard<std::mutex> wlock(write_mtx_);

#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

bool PipeClient::is_connected() const {
    return connected_;
}

std::vector<uint8_t> PipeClient::read() {
    return read_with_timeout(-1); // Blocking read
}

std::vector<uint8_t> PipeClient::read_with_timeout(int timeout_ms) {
    std::lock_guard<std::mutex> lock(read_mtx_);
    std::vector<uint8_t> result;

    if (!connected_) {
        return result;
    }

#ifdef _WIN32
    // Windows pipe read
    DWORD bytes_available = 0;
    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr)) {
        connected_ = false;
        return result;
    }

    if (bytes_available == 0 && timeout_ms == 0) {
        return result;
    }

    // Read up to 64KB at a time
    result.resize(std::min(bytes_available, (DWORD)65536));
    if (result.empty()) {
        result.resize(65536);
    }

    DWORD bytes_read = 0;
    if (!ReadFile(handle_, result.data(), (DWORD)result.size(), &bytes_read, nullptr)) {
        if (GetLastError() != ERROR_MORE_DATA) {
            connected_ = false;
            return {};
        }
    }

    result.resize(bytes_read);
#else
    // Unix socket read with poll for timeout
    if (timeout_ms >= 0) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) {
            if (ret < 0 && errno != EINTR) {
                connected_ = false;
            }
            return result;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            connected_ = false;
            return result;
        }
    }

    // Read up to 64KB at a time
    result.resize(65536);
    ssize_t bytes_read = ::read(fd_, result.data(), result.size());

    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            connected_ = false;
        }
        return {};
    }

    if (bytes_read == 0) {
        connected_ = false;
        return {};
    }

    result.resize(bytes_read);
#endif

    return result;
}

bool PipeClient::write(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(write_mtx_);

    if (!connected_ || data == nullptr || length == 0) {
        return false;
    }

#ifdef _WIN32
    size_t total_written = 0;
    while (total_written < length) {
        DWORD bytes_written = 0;
        if (!WriteFile(handle_, data + total_written, (DWORD)(length - total_written), &bytes_written, nullptr)) {
            connected_ = false;
            return false;
        }
        if (bytes_written == 0) {
            connected_ = false;
            return false;
        }
        total_written += bytes_written;
    }
    return true;
#else
    size_t total_written = 0;
    while (total_written < length) {
        ssize_t written = ::write(fd_, data + total_written, length - total_written);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            connected_ = false;
            return false;
        }
        if (written == 0) {
            connected_ = false;
            return false;
        }
        total_written += written;
    }
    return true;
#endif
}

bool PipeClient::write(const std::vector<uint8_t>& data) {
    return write(data.data(), data.size());
}

} // namespace bldr
