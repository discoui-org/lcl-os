#include "system/security/sandbox_protocol.hpp"

#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

class Writer final {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    bool string(const std::string& value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        const auto length = static_cast<std::uint16_t>(value.size());
        u8(static_cast<std::uint8_t>(length));
        u8(static_cast<std::uint8_t>(length >> 8));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    void digest(const Sha256Digest& value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader final {
public:
    Reader(const std::uint8_t* bytes, std::size_t size) : bytes_(bytes), size_(size) {}

    bool u8(std::uint8_t& value) {
        if (offset_ >= size_) {
            return false;
        }
        value = bytes_[offset_++];
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool string(std::string& value) {
        std::uint8_t low = 0;
        std::uint8_t high = 0;
        if (!u8(low) || !u8(high)) {
            return false;
        }
        const std::size_t length = static_cast<std::size_t>(low) |
                                   (static_cast<std::size_t>(high) << 8);
        if (length > size_ - offset_) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes_ + offset_), length);
        offset_ += length;
        return true;
    }
    bool digest(Sha256Digest& value) {
        if (value.size() > size_ - offset_) {
            return false;
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            value[index] = bytes_[offset_ + index];
        }
        offset_ += value.size();
        return true;
    }
    bool done() const noexcept { return offset_ == size_; }

private:
    const std::uint8_t* bytes_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
};

bool isValidOpcode(SandboxOpcode opcode) {
    return opcode >= SandboxOpcode::LaunchRequest && opcode <= SandboxOpcode::ErrorResponse;
}

bool isValidLaunchStatus(SandboxLaunchStatus status) {
    return status >= SandboxLaunchStatus::Launched && status <= SandboxLaunchStatus::ExecutionFailed;
}

} // namespace

bool encodeSandboxPacket(const SandboxHeader& header, const std::vector<std::uint8_t>& payload,
                         std::vector<std::uint8_t>& packet) {
    if (header.magic != kSandboxProtocolMagic || header.version != kSandboxProtocolVersion ||
        !isValidOpcode(header.opcode) || header.requestId == 0 ||
        payload.size() > kSandboxMaxPayload || header.payloadSize != payload.size()) {
        return false;
    }
    Writer writer;
    writer.u32(header.magic);
    writer.u32(header.version);
    writer.u32(static_cast<std::uint32_t>(header.opcode));
    writer.u32(header.requestId);
    writer.u32(header.payloadSize);
    packet = writer.take();
    packet.insert(packet.end(), payload.begin(), payload.end());
    return true;
}

bool decodeSandboxPacket(const std::uint8_t* packet, std::size_t packetSize,
                         DecodedSandboxPacket& decoded) {
    if (!packet || packetSize < kSandboxWireHeaderSize) {
        return false;
    }
    Reader reader(packet, kSandboxWireHeaderSize);
    std::uint32_t opcode = 0;
    if (!reader.u32(decoded.header.magic) || !reader.u32(decoded.header.version) ||
        !reader.u32(opcode) || !reader.u32(decoded.header.requestId) ||
        !reader.u32(decoded.header.payloadSize) || !reader.done()) {
        return false;
    }
    decoded.header.opcode = static_cast<SandboxOpcode>(opcode);
    if (decoded.header.magic != kSandboxProtocolMagic ||
        decoded.header.version != kSandboxProtocolVersion || !isValidOpcode(decoded.header.opcode) ||
        decoded.header.requestId == 0 || decoded.header.payloadSize > kSandboxMaxPayload ||
        packetSize != kSandboxWireHeaderSize + decoded.header.payloadSize) {
        return false;
    }
    decoded.payload.assign(packet + kSandboxWireHeaderSize, packet + packetSize);
    return true;
}

bool encodeSandboxLaunchRequest(const SandboxLaunchRequest& request,
                                std::vector<std::uint8_t>& payload) {
    std::string error;
    if (!validateSandboxLaunchRequest(request, error)) {
        return false;
    }
    Writer writer;
    writer.u32(request.contractVersion);
    writer.u64(request.instanceId);
    if (!writer.string(request.appId)) {
        return false;
    }
    writer.digest(request.bundleRecordDigest);
    writer.digest(request.profileDigest);
    payload = writer.take();
    return payload.size() <= kSandboxMaxPayload;
}

bool decodeSandboxLaunchRequest(const std::vector<std::uint8_t>& payload,
                                SandboxLaunchRequest& request) {
    Reader reader(payload.data(), payload.size());
    if (!reader.u32(request.contractVersion) || !reader.u64(request.instanceId) ||
        !reader.string(request.appId) || !reader.digest(request.bundleRecordDigest) ||
        !reader.digest(request.profileDigest) || !reader.done()) {
        return false;
    }
    std::string error;
    return validateSandboxLaunchRequest(request, error);
}

bool encodeSandboxLaunchResult(const SandboxLaunchResult& result,
                               std::vector<std::uint8_t>& payload) {
    if (!isValidLaunchStatus(result.status) || result.instanceId == 0 ||
        result.message.size() > 1024 ||
        (result.status == SandboxLaunchStatus::Launched &&
         (result.pid <= 0 || result.processGroupId <= 0))) {
        return false;
    }
    Writer writer;
    writer.u32(static_cast<std::uint32_t>(result.status));
    writer.u64(result.instanceId);
    writer.u32(std::bit_cast<std::uint32_t>(result.pid));
    writer.u32(std::bit_cast<std::uint32_t>(result.processGroupId));
    if (!writer.string(result.message)) {
        return false;
    }
    payload = writer.take();
    return payload.size() <= kSandboxMaxPayload;
}

bool decodeSandboxLaunchResult(const std::vector<std::uint8_t>& payload,
                               SandboxLaunchResult& result) {
    Reader reader(payload.data(), payload.size());
    std::uint32_t rawStatus = 0;
    std::uint32_t rawPid = 0;
    std::uint32_t rawProcessGroupId = 0;
    if (!reader.u32(rawStatus) || !reader.u64(result.instanceId) || !reader.u32(rawPid) ||
        !reader.u32(rawProcessGroupId) || !reader.string(result.message) || !reader.done()) {
        return false;
    }
    result.status = static_cast<SandboxLaunchStatus>(rawStatus);
    result.pid = std::bit_cast<std::int32_t>(rawPid);
    result.processGroupId = std::bit_cast<std::int32_t>(rawProcessGroupId);
    std::vector<std::uint8_t> canonical;
    return encodeSandboxLaunchResult(result, canonical);
}

} // namespace lcl::security
