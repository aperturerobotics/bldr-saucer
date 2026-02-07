#include "scheme_forwarder.h"

#include <cstring>
#include <iostream>

namespace bldr {

void SchemeForwarder::forward(const saucer::scheme::request& req,
                               saucer::scheme::stream_writer& writer) {
    // Open a new yamux stream.
    auto [stream, err] = session_->OpenStream();
    if (err != yamux::Error::OK || !stream) {
        std::cerr << "[forwarder] failed to open yamux stream" << std::endl;
        writer.start({.mime = "text/plain", .status = 502});
        writer.finish();
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
        std::cerr << "[forwarder] failed to write request info" << std::endl;
        stream->Close();
        writer.start({.mime = "text/plain", .status = 502});
        writer.finish();
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
            std::cerr << "[forwarder] failed to write request body" << std::endl;
            stream->Close();
            writer.start({.mime = "text/plain", .status = 502});
            writer.finish();
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
                writer.start({.mime = "text/plain", .status = 502});
            }
            break;
        }

        proto::FetchResponse resp;
        if (!proto::DecodeFetchResponse(frame.data(), frame.size(), resp)) {
            std::cerr << "[forwarder] failed to decode response" << std::endl;
            if (!started) {
                writer.start({.mime = "text/plain", .status = 502});
            }
            break;
        }

        // Process ResponseInfo (first frame).
        if (resp.has_info && !started) {
            started = true;

            // Determine MIME type from Content-Type header.
            std::string mime = "application/octet-stream";
            auto it = resp.info.headers.find("Content-Type");
            if (it == resp.info.headers.end()) {
                it = resp.info.headers.find("content-type");
            }
            if (it != resp.info.headers.end()) {
                mime = it->second;
            }

            // Build response headers for saucer (excluding Content-Type which goes in mime).
            std::map<std::string, std::string> hdrs;
            for (const auto& [key, val] : resp.info.headers) {
                if (key != "Content-Type" && key != "content-type") {
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

    writer.finish();
    stream->Close();
}

bool SchemeForwarder::writeFrame(yamux::Stream* stream, const std::vector<uint8_t>& data) {
    // Write LittleEndian uint32 length prefix.
    uint8_t lenBuf[4];
    uint32_t msgLen = static_cast<uint32_t>(data.size());
    std::memcpy(lenBuf, &msgLen, 4); // LE on LE platforms

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
    std::memcpy(&msgLen, lenBuf, 4); // LE on LE platforms

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
