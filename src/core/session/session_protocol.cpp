#include "core/session/session_protocol.hpp"

#include <algorithm>
#include <bit>

namespace lcl::session {
namespace {

class Writer {
public:
    void u8(uint8_t value) { m_bytes.push_back(value); }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) u8(static_cast<uint8_t>(value >> shift));
    }
    void u64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) u8(static_cast<uint8_t>(value >> shift));
    }
    bool string(const std::string& value) {
        if (value.size() > UINT16_MAX) return false;
        const auto size = static_cast<uint16_t>(value.size());
        u8(static_cast<uint8_t>(size));
        u8(static_cast<uint8_t>(size >> 8));
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return true;
    }
    std::vector<uint8_t> take() { return std::move(m_bytes); }
private:
    std::vector<uint8_t> m_bytes;
};

class Reader {
public:
    Reader(const uint8_t* bytes, size_t size) : m_bytes(bytes), m_size(size) {}
    bool u8(uint8_t& value) {
        if (m_offset >= m_size) return false;
        value = m_bytes[m_offset++];
        return true;
    }
    bool u32(uint32_t& value) {
        uint8_t byte[4]{};
        if (!u8(byte[0]) || !u8(byte[1]) || !u8(byte[2]) || !u8(byte[3])) return false;
        value = static_cast<uint32_t>(byte[0]) | (static_cast<uint32_t>(byte[1]) << 8) |
                (static_cast<uint32_t>(byte[2]) << 16) | (static_cast<uint32_t>(byte[3]) << 24);
        return true;
    }
    bool u64(uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<uint64_t>(byte) << shift;
        }
        return true;
    }
    bool string(std::string& value) {
        uint8_t lo = 0, hi = 0;
        if (!u8(lo) || !u8(hi)) return false;
        const size_t length = static_cast<size_t>(lo) | (static_cast<size_t>(hi) << 8);
        if (length > m_size - m_offset) return false;
        value.assign(reinterpret_cast<const char*>(m_bytes + m_offset), length);
        m_offset += length;
        return true;
    }
    bool done() const { return m_offset == m_size; }
private:
    const uint8_t* m_bytes;
    size_t m_size;
    size_t m_offset{0};
};

bool validOpcode(SessionOpcode opcode) {
    return opcode >= SessionOpcode::CatalogRequest && opcode <= SessionOpcode::ErrorResponse;
}

bool encodeString(const std::string& value, std::vector<uint8_t>& payload) {
    Writer writer;
    if (!writer.string(value)) return false;
    payload = writer.take();
    return true;
}

bool decodeString(const std::vector<uint8_t>& payload, std::string& value) {
    Reader reader(payload.data(), payload.size());
    return reader.string(value) && reader.done();
}

} // namespace

bool encodePacket(const SessionHeader& header, const std::vector<uint8_t>& payload,
                  std::vector<uint8_t>& packet) {
    if (header.magic != kSessionProtocolMagic || header.version != kSessionProtocolVersion ||
        !validOpcode(header.opcode) || header.requestId == 0 ||
        payload.size() > kSessionMaxPayload || header.payloadSize != payload.size()) return false;
    Writer writer;
    writer.u32(header.magic);
    writer.u32(header.version);
    writer.u32(static_cast<uint32_t>(header.opcode));
    writer.u32(header.requestId);
    writer.u32(header.payloadSize);
    packet = writer.take();
    packet.insert(packet.end(), payload.begin(), payload.end());
    return true;
}

bool decodePacket(const uint8_t* packet, size_t packetSize, DecodedPacket& decoded) {
    if (!packet || packetSize < kSessionWireHeaderSize) return false;
    Reader headerReader(packet, kSessionWireHeaderSize);
    uint32_t opcode = 0;
    if (!headerReader.u32(decoded.header.magic) || !headerReader.u32(decoded.header.version) ||
        !headerReader.u32(opcode) || !headerReader.u32(decoded.header.requestId) ||
        !headerReader.u32(decoded.header.payloadSize) || !headerReader.done()) return false;
    decoded.header.opcode = static_cast<SessionOpcode>(opcode);
    if (decoded.header.magic != kSessionProtocolMagic ||
        decoded.header.version != kSessionProtocolVersion || !validOpcode(decoded.header.opcode) ||
        decoded.header.requestId == 0 || decoded.header.payloadSize > kSessionMaxPayload ||
        packetSize != kSessionWireHeaderSize + decoded.header.payloadSize) return false;
    decoded.payload.assign(packet + kSessionWireHeaderSize, packet + packetSize);
    return true;
}

bool encodeCatalogSnapshot(const std::vector<CatalogEntry>& entries, std::vector<uint8_t>& payload) {
    if (entries.size() > UINT32_MAX) return false;
    Writer writer;
    writer.u32(static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!writer.string(entry.appId) || !writer.string(entry.name) ||
            !writer.string(entry.version) || !writer.string(entry.icon) ||
            !writer.string(entry.type)) return false;
    }
    payload = writer.take();
    return payload.size() <= kSessionMaxPayload;
}

bool decodeCatalogSnapshot(const std::vector<uint8_t>& payload, std::vector<CatalogEntry>& entries) {
    Reader reader(payload.data(), payload.size());
    uint32_t count = 0;
    if (!reader.u32(count) || count > 4096) return false;
    entries.clear();
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        CatalogEntry entry;
        if (!reader.string(entry.appId) || !reader.string(entry.name) ||
            !reader.string(entry.version) || !reader.string(entry.icon) ||
            !reader.string(entry.type) || entry.appId.empty()) return false;
        entries.push_back(std::move(entry));
    }
    return reader.done();
}

bool encodeLaunchRequest(const LaunchRequest& request, std::vector<uint8_t>& payload) {
    if (request.target.empty()) return false;
    Writer writer;
    writer.u8(request.waitForExit ? 1 : 0);
    if (!writer.string(request.target)) return false;
    payload = writer.take();
    return true;
}

bool decodeLaunchRequest(const std::vector<uint8_t>& payload, LaunchRequest& request) {
    Reader reader(payload.data(), payload.size());
    uint8_t wait = 0;
    if (!reader.u8(wait) || wait > 1 || !reader.string(request.target) ||
        request.target.empty() || !reader.done()) return false;
    request.waitForExit = wait != 0;
    return true;
}

bool encodeLaunchResponse(const LaunchResponse& response, std::vector<uint8_t>& payload) {
    Writer writer;
    writer.u32(response.status);
    writer.u64(response.instanceId);
    writer.u32(std::bit_cast<uint32_t>(response.pid));
    if (!writer.string(response.appId) || !writer.string(response.message)) return false;
    payload = writer.take();
    return true;
}

bool decodeLaunchResponse(const std::vector<uint8_t>& payload, LaunchResponse& response) {
    Reader reader(payload.data(), payload.size());
    uint32_t pid = 0;
    if (!reader.u32(response.status) || !reader.u64(response.instanceId) || !reader.u32(pid) ||
        !reader.string(response.appId) || !reader.string(response.message) || !reader.done()) return false;
    response.pid = std::bit_cast<int32_t>(pid);
    return true;
}

bool encodeProcessExited(const ProcessExited& event, std::vector<uint8_t>& payload) {
    Writer writer;
    writer.u64(event.instanceId);
    writer.u32(std::bit_cast<uint32_t>(event.exitCode));
    payload = writer.take();
    return true;
}

bool decodeProcessExited(const std::vector<uint8_t>& payload, ProcessExited& event) {
    Reader reader(payload.data(), payload.size());
    uint32_t status = 0;
    if (!reader.u64(event.instanceId) || !reader.u32(status) || !reader.done()) return false;
    event.exitCode = std::bit_cast<int32_t>(status);
    return true;
}

bool encodeError(const std::string& message, std::vector<uint8_t>& payload) {
    return encodeString(message, payload);
}

bool decodeError(const std::vector<uint8_t>& payload, std::string& message) {
    return decodeString(payload, message);
}

} // namespace lcl::session
