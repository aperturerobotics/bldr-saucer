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

// corsHeaders are Access-Control headers added to all scheme responses.
// WebKit treats custom scheme origins as opaque (null), so all fetch requests
// from pages loaded via bldr:// are cross-origin. These headers allow them.
static const std::map<std::string, std::string> corsHeaders = {
    {"Access-Control-Allow-Origin", "*"},
    {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    {"Access-Control-Allow-Headers", "*"},
};

// sendError resolves the executor with an error status response.
static void sendError(saucer::scheme::executor& executor, int status) {
    executor.resolve({
        .data = saucer::stash::empty(),
        .mime = "text/plain",
        .headers = corsHeaders,
        .status = status,
    });
}

void SchemeForwarder::forward(const saucer::scheme::request& req,
                               saucer::scheme::executor& executor) {
    // Handle CORS preflight directly without forwarding to Go.
    if (toLower(req.method()) == "options") {
        executor.resolve({
            .data = saucer::stash::empty(),
            .mime = "text/plain",
            .headers = corsHeaders,
            .status = 204,
        });
        return;
    }

    // Open a new yamux stream.
    auto [stream, err] = session_->OpenStream();
    if (err != yamux::Error::OK || !stream) {
        sendError(executor, 502);
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
    auto content = req.content().data();
    info.has_body = (content.size() > 0);

    // Serialize and send FetchRequestInfo frame.
    auto reqInfoMsg = proto::EncodeFetchRequest_Info(info);
    if (!writeFrame(stream.get(), reqInfoMsg)) {
        stream->Close();
        sendError(executor, 502);
        return;
    }

    // Send body if present.
    if (info.has_body) {
        proto::FetchRequestData bodyData;
        bodyData.data.assign(content.data(), content.data() + content.size());
        bodyData.done = true;

        auto reqDataMsg = proto::EncodeFetchRequest_Data(bodyData);
        if (!writeFrame(stream.get(), reqDataMsg)) {
            stream->Close();
            sendError(executor, 502);
            return;
        }
    }

    // Create streaming stash for incremental response delivery.
    auto result = saucer::scheme::response::stream();
    if (!result) {
        executor.reject(saucer::scheme::error::failed);
        stream->Close();
        return;
    }
    auto [stash, write] = std::move(*result);

    // Read response frames from Go.
    bool resolved = false;
    bool done = false;

    while (!done) {
        std::vector<uint8_t> frame;
        if (!readFrame(stream.get(), frame)) {
            if (!resolved) {
                executor.reject(saucer::scheme::error::failed);
            }
            break;
        }

        proto::FetchResponse resp;
        if (!proto::DecodeFetchResponse(frame.data(), frame.size(), resp)) {
            if (!resolved) {
                executor.reject(saucer::scheme::error::failed);
            }
            break;
        }

        // Process ResponseInfo (first frame): resolve executor with headers and streaming stash.
        if (resp.has_info && !resolved) {
            resolved = true;

            // Extract Content-Type header (case-insensitive) and merge CORS headers.
            std::string mime = "application/octet-stream";
            std::map<std::string, std::string> hdrs(corsHeaders);
            for (const auto& [key, val] : resp.info.headers) {
                if (toLower(key) == "content-type") {
                    mime = val;
                } else {
                    hdrs[key] = val;
                }
            }

            executor.resolve({
                .data = std::move(stash),
                .mime = mime,
                .headers = hdrs,
                .status = static_cast<int>(resp.info.status),
            });
        }

        // Process ResponseData: push body chunks via streaming write callback.
        if (resp.has_data) {
            if (!resolved) {
                resolved = true;
                executor.resolve({
                    .data = std::move(stash),
                    .mime = "application/octet-stream",
                    .status = 200,
                });
            }

            if (!resp.data.data.empty()) {
                if (!write({resp.data.data.data(), resp.data.data.size()})) {
                    break;
                }
            }

            if (resp.data.done) {
                done = true;
            }
        }
    }

    // Destroying write closes the streaming stash.
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
