#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bldr {
namespace proto {

// SaucerInit corresponds to saucer.SaucerInit.
// Passed from Go as base64-encoded protobuf via BLDR_SAUCER_INIT env var.
struct SaucerInit {
    bool dev_tools = false;      // field 1
    uint32_t external_links = 0; // field 2 (enum ExternalLinks)
};

// DecodeSaucerInit decodes a SaucerInit protobuf message.
bool DecodeSaucerInit(const uint8_t* buf, size_t len, SaucerInit& out);

// Base64Decode decodes a base64-encoded string.
std::vector<uint8_t> Base64Decode(const std::string& input);

// FetchRequestInfo corresponds to web.fetch.FetchRequestInfo.
struct FetchRequestInfo {
    std::string method;                        // field 1
    std::string url;                           // field 2
    std::map<std::string, std::string> headers; // field 3
    bool has_body = false;                     // field 4
};

// FetchRequestData corresponds to web.fetch.FetchRequestData.
struct FetchRequestData {
    std::vector<uint8_t> data; // field 1
    bool done = false;         // field 2
};

// ResponseInfo corresponds to web.fetch.ResponseInfo.
struct ResponseInfo {
    std::map<std::string, std::string> headers; // field 1
    bool ok = false;                            // field 2
    uint32_t status = 0;                        // field 4
    std::string status_text;                    // field 5
};

// ResponseData corresponds to web.fetch.ResponseData.
struct ResponseData {
    std::vector<uint8_t> data; // field 1
    bool done = false;         // field 2
};

// FetchResponse holds a decoded FetchResponse.
struct FetchResponse {
    bool has_info = false;
    ResponseInfo info;
    bool has_data = false;
    ResponseData data;
};

// EncodeFetchRequest_Info serializes a FetchRequest with request_info (field 1).
std::vector<uint8_t> EncodeFetchRequest_Info(const FetchRequestInfo& info);

// EncodeFetchRequest_Data serializes a FetchRequest with request_data (field 2).
std::vector<uint8_t> EncodeFetchRequest_Data(const FetchRequestData& data);

// DecodeFetchResponse decodes a FetchResponse message.
bool DecodeFetchResponse(const uint8_t* buf, size_t len, FetchResponse& out);

} // namespace proto
} // namespace bldr
