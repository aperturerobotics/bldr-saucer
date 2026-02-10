#include "scheme_forwarder.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bldr {

// toLower returns a lowercase copy of the string.
static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// sendError sends an error response to the saucer writer.
static void sendError(saucer::scheme::stream_writer& writer, int status) {
    writer.start({.mime = "text/plain", .status = status});
    writer.finish();
}

void SchemeForwarder::forward(const saucer::scheme::request& req,
                               saucer::scheme::stream_writer& writer) {
    // Open a new yamux stream.
    auto [stream, err] = session_->OpenStream();
    if (err != yamux::Error::OK || !stream) {
        sendError(writer, 502);
        return;
    }

    // Build FetchRequestInfo from the scheme request.
    proto::FetchRequestInfo info;
    info.method = req.method();
    info.url = req.url().string();

    // Copy request headers.
    for (const auto& [key, val] : req.headers()) {
        info.headers[key] = val;
    }

    // Check if request has a body.
    auto content = req.content();
    info.has_body = (content.size() > 0);

    // Serialize and send FetchRequestInfo frame.
    auto reqInfoMsg = proto::EncodeFetchRequest_Info(info);
    if (!writeFrame(stream.get(), reqInfoMsg)) {
        stream->Close();
        sendError(writer, 502);
        return;
    }

    // Send body if present.
    if (info.has_body) {
        proto::FetchRequestData bodyData;
        bodyData.data.assign(
            static_cast<const uint8_t*>(content.data()),
            static_cast<const uint8_t*>(content.data()) + content.size()
        );
        bodyData.done = true;

        auto reqDataMsg = proto::EncodeFetchRequest_Data(bodyData);
        if (!writeFrame(stream.get(), reqDataMsg)) {
            stream->Close();
            sendError(writer, 502);
            return;
        }
    }

    // Read response frames from Go.
    bool started = false;
    bool done = false;

    while (!done) {
        std::vector<uint8_t> frame;
        if (!readFrame(stream.get(), frame)) {
            if (!started) {
                sendError(writer, 502);
            }
            break;
        }

        proto::FetchResponse resp;
        if (!proto::DecodeFetchResponse(frame.data(), frame.size(), resp)) {
            if (!started) {
                sendError(writer, 502);
            }
            break;
        }

        // Process ResponseInfo (first frame).
        if (resp.has_info && !started) {
            started = true;

            // Extract Content-Type header (case-insensitive).
            std::string mime = "application/octet-stream";
            std::map<std::string, std::string> hdrs;
            for (const auto& [key, val] : resp.info.headers) {
                if (toLower(key) == "content-type") {
                    mime = val;
                } else {
                    hdrs[key] = val;
                }
            }

            writer.start({
                .mime = mime,
                .headers = hdrs,
                .status = static_cast<int>(resp.info.status),
            });
        }

        // Process ResponseData.
        if (resp.has_data) {
            if (!started) {
                started = true;
                writer.start({.mime = "application/octet-stream", .status = 200});
            }

            if (!resp.data.data.empty() && writer.valid()) {
                writer.write(saucer::stash::from(std::move(resp.data.data)));
            }

            if (resp.data.done) {
                done = true;
            }
        }
    }

    if (started) {
        writer.finish();
    }
    stream->Close();
}

bool SchemeForwarder::writeFrame(yamux::Stream* stream, const std::vector<uint8_t>& data) {
    // Write LittleEndian uint32 length prefix.
    uint8_t lenBuf[4];
    uint32_t msgLen = static_cast<uint32_t>(data.size());
    std::memcpy(lenBuf, &msgLen, 4); // LE on LE platforms (x86_64, ARM64)

    auto err = stream->Write(lenBuf, 4);
    if (err != yamux::Error::OK) return false;

    err = stream->Write(data.data(), data.size());
    return err == yamux::Error::OK;
}

bool SchemeForwarder::readFrame(yamux::Stream* stream, std::vector<uint8_t>& out) {
    // Read LittleEndian uint32 length prefix.
    uint8_t lenBuf[4];
    size_t total = 0;
    while (total < 4) {
        auto [n, err] = stream->Read(lenBuf + total, 4 - total);
        if (err != yamux::Error::OK || n == 0) return false;
        total += n;
    }

    uint32_t msgLen;
    std::memcpy(&msgLen, lenBuf, 4); // LE on LE platforms (x86_64, ARM64)

    if (msgLen > kMaxFrameSize) return false;

    out.resize(msgLen);
    total = 0;
    while (total < msgLen) {
        auto [n, err] = stream->Read(out.data() + total, msgLen - total);
        if (err != yamux::Error::OK || n == 0) return false;
        total += n;
    }

    return true;
}

} // namespace bldr
