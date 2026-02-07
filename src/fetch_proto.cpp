#include "fetch_proto.h"
#include <cstring>

namespace bldr {
namespace proto {

// Base64 decoding table.
static constexpr int8_t kBase64Table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

std::vector<uint8_t> Base64Decode(const std::string& input) {
    std::vector<uint8_t> out;
    out.reserve(input.size() * 3 / 4);
    uint32_t accum = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int8_t val = kBase64Table[static_cast<uint8_t>(c)];
        if (val < 0) continue;
        accum = (accum << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    return out;
}

// Protobuf wire type constants.
static constexpr uint8_t kVarint = 0;
static constexpr uint8_t kLengthDelimited = 2;

// encodeVarint appends a varint to the buffer.
static void encodeVarint(std::vector<uint8_t>& buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>(val | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val));
}

// encodeTag appends a field tag.
static void encodeTag(std::vector<uint8_t>& buf, uint32_t field, uint8_t wire) {
    encodeVarint(buf, (static_cast<uint64_t>(field) << 3) | wire);
}

// encodeString appends a length-delimited string field.
static void encodeString(std::vector<uint8_t>& buf, uint32_t field, const std::string& val) {
    if (val.empty()) return;
    encodeTag(buf, field, kLengthDelimited);
    encodeVarint(buf, val.size());
    buf.insert(buf.end(), val.begin(), val.end());
}

// encodeBytes appends a length-delimited bytes field.
static void encodeBytes(std::vector<uint8_t>& buf, uint32_t field, const std::vector<uint8_t>& val) {
    if (val.empty()) return;
    encodeTag(buf, field, kLengthDelimited);
    encodeVarint(buf, val.size());
    buf.insert(buf.end(), val.begin(), val.end());
}

// encodeBool appends a bool varint field.
static void encodeBool(std::vector<uint8_t>& buf, uint32_t field, bool val) {
    if (!val) return;
    encodeTag(buf, field, kVarint);
    buf.push_back(1);
}

// encodeUint32 appends a uint32 varint field.
static void encodeUint32(std::vector<uint8_t>& buf, uint32_t field, uint32_t val) {
    if (val == 0) return;
    encodeTag(buf, field, kVarint);
    encodeVarint(buf, val);
}

// encodeMapEntry encodes a map<string,string> entry as a sub-message.
// Map entry: key=field 1 (string), value=field 2 (string).
static void encodeMapEntry(std::vector<uint8_t>& buf, uint32_t field,
                           const std::string& key, const std::string& value) {
    // Build the sub-message for the map entry.
    std::vector<uint8_t> entry;
    encodeString(entry, 1, key);
    encodeString(entry, 2, value);

    // Write as length-delimited sub-message.
    encodeTag(buf, field, kLengthDelimited);
    encodeVarint(buf, entry.size());
    buf.insert(buf.end(), entry.begin(), entry.end());
}

// encodeLengthDelimitedMsg wraps a sub-message as a field.
static void encodeLengthDelimitedMsg(std::vector<uint8_t>& buf, uint32_t field,
                                     const std::vector<uint8_t>& msg) {
    encodeTag(buf, field, kLengthDelimited);
    encodeVarint(buf, msg.size());
    buf.insert(buf.end(), msg.begin(), msg.end());
}

// encodeFetchRequestInfo encodes a FetchRequestInfo sub-message.
static std::vector<uint8_t> encodeFetchRequestInfo(const FetchRequestInfo& info) {
    std::vector<uint8_t> buf;
    encodeString(buf, 1, info.method);
    encodeString(buf, 2, info.url);
    for (const auto& [key, val] : info.headers) {
        encodeMapEntry(buf, 3, key, val);
    }
    encodeBool(buf, 4, info.has_body);
    return buf;
}

// encodeFetchRequestData encodes a FetchRequestData sub-message.
static std::vector<uint8_t> encodeFetchRequestData(const FetchRequestData& data) {
    std::vector<uint8_t> buf;
    encodeBytes(buf, 1, data.data);
    encodeBool(buf, 2, data.done);
    return buf;
}

std::vector<uint8_t> EncodeFetchRequest_Info(const FetchRequestInfo& info) {
    // FetchRequest: oneof body { request_info = 1; }
    std::vector<uint8_t> buf;
    auto sub = encodeFetchRequestInfo(info);
    encodeLengthDelimitedMsg(buf, 1, sub);
    return buf;
}

std::vector<uint8_t> EncodeFetchRequest_Data(const FetchRequestData& data) {
    // FetchRequest: oneof body { request_data = 2; }
    std::vector<uint8_t> buf;
    auto sub = encodeFetchRequestData(data);
    encodeLengthDelimitedMsg(buf, 2, sub);
    return buf;
}

// decodeVarint reads a varint from buf at offset, updates offset.
static bool decodeVarint(const uint8_t* buf, size_t len, size_t& offset, uint64_t& val) {
    val = 0;
    int shift = 0;
    while (offset < len) {
        uint8_t b = buf[offset++];
        val |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

// decodeTag reads a field tag and wire type.
static bool decodeTag(const uint8_t* buf, size_t len, size_t& offset,
                      uint32_t& field, uint8_t& wire) {
    uint64_t tag;
    if (!decodeVarint(buf, len, offset, tag)) return false;
    field = static_cast<uint32_t>(tag >> 3);
    wire = static_cast<uint8_t>(tag & 0x07);
    return true;
}

// skipField skips over a field value based on wire type.
static bool skipField(const uint8_t* buf, size_t len, size_t& offset, uint8_t wire) {
    switch (wire) {
        case 0: { // varint
            uint64_t dummy;
            return decodeVarint(buf, len, offset, dummy);
        }
        case 1: // 64-bit
            offset += 8;
            return offset <= len;
        case 2: { // length-delimited
            uint64_t slen;
            if (!decodeVarint(buf, len, offset, slen)) return false;
            offset += slen;
            return offset <= len;
        }
        case 5: // 32-bit
            offset += 4;
            return offset <= len;
        default:
            return false;
    }
}

// decodeLengthDelimited reads a length-delimited field returning a span.
static bool decodeLengthDelimited(const uint8_t* buf, size_t len, size_t& offset,
                                  const uint8_t*& data, size_t& dlen) {
    uint64_t slen;
    if (!decodeVarint(buf, len, offset, slen)) return false;
    if (offset + slen > len) return false;
    data = buf + offset;
    dlen = static_cast<size_t>(slen);
    offset += slen;
    return true;
}

// decodeString reads a string from a length-delimited field.
static bool decodeString(const uint8_t* buf, size_t len, size_t& offset, std::string& out) {
    const uint8_t* data;
    size_t dlen;
    if (!decodeLengthDelimited(buf, len, offset, data, dlen)) return false;
    out.assign(reinterpret_cast<const char*>(data), dlen);
    return true;
}

// decodeResponseInfo decodes a ResponseInfo sub-message.
static bool decodeResponseInfo(const uint8_t* buf, size_t len, ResponseInfo& out) {
    size_t offset = 0;
    while (offset < len) {
        uint32_t field;
        uint8_t wire;
        if (!decodeTag(buf, len, offset, field, wire)) return false;

        switch (field) {
            case 1: { // headers map
                if (wire != kLengthDelimited) return false;
                const uint8_t* entry;
                size_t elen;
                if (!decodeLengthDelimited(buf, len, offset, entry, elen)) return false;
                // Decode map entry sub-message.
                size_t eoff = 0;
                std::string key, val;
                while (eoff < elen) {
                    uint32_t ef;
                    uint8_t ew;
                    if (!decodeTag(entry, elen, eoff, ef, ew)) return false;
                    if (ef == 1 && ew == kLengthDelimited) {
                        if (!decodeString(entry, elen, eoff, key)) return false;
                    } else if (ef == 2 && ew == kLengthDelimited) {
                        if (!decodeString(entry, elen, eoff, val)) return false;
                    } else {
                        if (!skipField(entry, elen, eoff, ew)) return false;
                    }
                }
                if (!key.empty()) out.headers[key] = val;
                break;
            }
            case 2: { // ok
                if (wire != kVarint) return false;
                uint64_t v;
                if (!decodeVarint(buf, len, offset, v)) return false;
                out.ok = (v != 0);
                break;
            }
            case 4: { // status
                if (wire != kVarint) return false;
                uint64_t v;
                if (!decodeVarint(buf, len, offset, v)) return false;
                out.status = static_cast<uint32_t>(v);
                break;
            }
            case 5: { // status_text
                if (wire != kLengthDelimited) return false;
                if (!decodeString(buf, len, offset, out.status_text)) return false;
                break;
            }
            default:
                if (!skipField(buf, len, offset, wire)) return false;
                break;
        }
    }
    return true;
}

// decodeResponseData decodes a ResponseData sub-message.
static bool decodeResponseData(const uint8_t* buf, size_t len, ResponseData& out) {
    size_t offset = 0;
    while (offset < len) {
        uint32_t field;
        uint8_t wire;
        if (!decodeTag(buf, len, offset, field, wire)) return false;

        switch (field) {
            case 1: { // data
                if (wire != kLengthDelimited) return false;
                const uint8_t* data;
                size_t dlen;
                if (!decodeLengthDelimited(buf, len, offset, data, dlen)) return false;
                out.data.assign(data, data + dlen);
                break;
            }
            case 2: { // done
                if (wire != kVarint) return false;
                uint64_t v;
                if (!decodeVarint(buf, len, offset, v)) return false;
                out.done = (v != 0);
                break;
            }
            default:
                if (!skipField(buf, len, offset, wire)) return false;
                break;
        }
    }
    return true;
}

bool DecodeFetchResponse(const uint8_t* buf, size_t len, FetchResponse& out) {
    // FetchResponse: oneof body { response_info = 1; response_data = 2; }
    size_t offset = 0;
    while (offset < len) {
        uint32_t field;
        uint8_t wire;
        if (!decodeTag(buf, len, offset, field, wire)) return false;

        switch (field) {
            case 1: { // response_info
                if (wire != kLengthDelimited) return false;
                const uint8_t* sub;
                size_t slen;
                if (!decodeLengthDelimited(buf, len, offset, sub, slen)) return false;
                out.has_info = true;
                if (!decodeResponseInfo(sub, slen, out.info)) return false;
                break;
            }
            case 2: { // response_data
                if (wire != kLengthDelimited) return false;
                const uint8_t* sub;
                size_t slen;
                if (!decodeLengthDelimited(buf, len, offset, sub, slen)) return false;
                out.has_data = true;
                if (!decodeResponseData(sub, slen, out.data)) return false;
                break;
            }
            default:
                if (!skipField(buf, len, offset, wire)) return false;
                break;
        }
    }
    return true;
}

bool DecodeSaucerInit(const uint8_t* buf, size_t len, SaucerInit& out) {
    size_t offset = 0;
    while (offset < len) {
        uint32_t field;
        uint8_t wire;
        if (!decodeTag(buf, len, offset, field, wire)) return false;

        switch (field) {
            case 1: { // dev_tools
                if (wire != kVarint) return false;
                uint64_t v;
                if (!decodeVarint(buf, len, offset, v)) return false;
                out.dev_tools = (v != 0);
                break;
            }
            case 2: { // external_links
                if (wire != kVarint) return false;
                uint64_t v;
                if (!decodeVarint(buf, len, offset, v)) return false;
                out.external_links = static_cast<uint32_t>(v);
                break;
            }
            default:
                if (!skipField(buf, len, offset, wire)) return false;
                break;
        }
    }
    return true;
}

} // namespace proto
} // namespace bldr
