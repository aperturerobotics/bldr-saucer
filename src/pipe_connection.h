#pragma once

#include "pipe_client.h"
#include "yamux/connection.hpp"

#include <cstring>
#include <vector>

namespace bldr {

// PipeConnection adapts PipeClient to the yamux::Connection interface.
// Buffers excess data from pipe reads.
class PipeConnection : public yamux::Connection {
public:
    explicit PipeConnection(PipeClient& pipe) : pipe_(pipe) {}

    yamux::Error Write(const uint8_t* data, size_t len) override {
        if (!pipe_.write(data, len)) {
            return yamux::Error::ConnectionReset;
        }
        return yamux::Error::OK;
    }

    yamux::Result<size_t> Read(uint8_t* buf, size_t max_len) override {
        // Serve from buffer first.
        if (!buf_.empty()) {
            size_t n = std::min(buf_.size(), max_len);
            std::memcpy(buf, buf_.data(), n);
            buf_.erase(buf_.begin(), buf_.begin() + n);
            return {n, yamux::Error::OK};
        }

        // Read from pipe.
        auto data = pipe_.read();
        if (data.empty()) {
            if (!pipe_.is_connected()) {
                return {0, yamux::Error::ConnectionReset};
            }
            return {0, yamux::Error::EOF_};
        }

        size_t n = std::min(data.size(), max_len);
        std::memcpy(buf, data.data(), n);

        // Buffer the excess.
        if (n < data.size()) {
            buf_.assign(data.begin() + n, data.end());
        }

        return {n, yamux::Error::OK};
    }

    yamux::Error Close() override {
        pipe_.close();
        return yamux::Error::OK;
    }

    bool IsClosed() const override {
        return !pipe_.is_connected();
    }

private:
    PipeClient& pipe_;
    std::vector<uint8_t> buf_;
};

} // namespace bldr
