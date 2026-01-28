#pragma once

#include "fetch_proto.h"
#include "yamux/session.hpp"

#include <saucer/smartview.hpp>

#include <cstdint>
#include <memory>

namespace bldr {

// MaxFrameSize is the maximum size of a length-prefixed frame (10MB).
static constexpr uint32_t kMaxFrameSize = 10 * 1024 * 1024;

// SchemeForwarder forwards saucer scheme requests to Go over yamux.
// Each request opens a new yamux stream and exchanges FetchRequest/FetchResponse
// frames using LittleEndian uint32 length-prefix framing.
class SchemeForwarder {
public:
    explicit SchemeForwarder(yamux::Session* session) : session_(session) {}

    // forward handles a single scheme request by forwarding it to Go.
    void forward(const saucer::scheme::request& req, saucer::scheme::stream_writer& writer);

private:
    // writeFrame writes a length-prefixed frame to a yamux stream.
    bool writeFrame(yamux::Stream* stream, const std::vector<uint8_t>& data);

    // readFrame reads a length-prefixed frame from a yamux stream.
    bool readFrame(yamux::Stream* stream, std::vector<uint8_t>& out);

    yamux::Session* session_;
};

} // namespace bldr
