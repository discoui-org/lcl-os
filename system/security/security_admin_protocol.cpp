#include "system/security/security_admin_protocol.hpp"

#include "system/security/app_identity_registry.hpp"

#include <limits>
#include <string_view>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kMaximumPendingRecords = 256;
constexpr std::size_t kMaximumBundlePathBytes = 4096;
constexpr std::size_t kMaximumDisplayNameBytes = 256;
constexpr std::size_t kMaximumVersionBytes = 128;
constexpr std::size_t kMaximumDiagnosticBytes = 1024;

class Writer final {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    bool string(const std::string& value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        const auto size = static_cast<std::uint16_t>(value.size());
        u8(static_cast<std::uint8_t>(size));
        u8(static_cast<std::uint8_t>(size >> 8));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    void digest(const Sha256Digest& digest) {
        bytes_.insert(bytes_.end(), digest.begin(), digest.end());
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

bool isVisibleText(std::string_view value, std::size_t maximumLength, bool permitEmpty = false) {
    if ((!permitEmpty && value.empty()) || value.size() > maximumLength) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7F) {
            return false;
        }
    }
    return true;
}

bool isValidOpcode(SecurityAdminOpcode opcode) {
    return opcode >= SecurityAdminOpcode::PendingBundleApprovalsRequest &&
           opcode <= SecurityAdminOpcode::ErrorResponse;
}

bool isValidIdentity(const SecurityAdminBundleIdentity& identity) {
    return AppIdentityRegistry::isValidAppId(identity.appId) &&
           !isZeroDigest(identity.bundleRecordDigest);
}

bool isValidPendingApproval(const PendingBundleApproval& request) {
    return request.userUid != 0 && AppIdentityRegistry::isValidAppId(request.appId) &&
           !isZeroDigest(request.bundleRecordDigest) &&
           request.publisherState == BundlePublisherState::Unverified &&
           request.sourceScope != BundleSourceScope::System &&
           isVisibleText(request.bundlePath, kMaximumBundlePathBytes) &&
           request.bundlePath.front() == '/' &&
           isVisibleText(request.displayName, kMaximumDisplayNameBytes) &&
           isVisibleText(request.appVersion, kMaximumVersionBytes, true);
}

} // namespace

bool encodeSecurityAdminPacket(const SecurityAdminHeader& header,
                               const std::vector<std::uint8_t>& payload,
                               std::vector<std::uint8_t>& packet) {
    if (header.magic != kSecurityAdminProtocolMagic ||
        header.version != kSecurityAdminProtocolVersion || !isValidOpcode(header.opcode) ||
        header.requestId == 0 || header.payloadSize != payload.size() ||
        payload.size() > kSecurityAdminMaxPayload) {
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

bool decodeSecurityAdminPacket(const std::uint8_t* packet, std::size_t packetSize,
                               DecodedSecurityAdminPacket& decoded) {
    if (!packet || packetSize < kSecurityAdminWireHeaderSize) {
        return false;
    }
    Reader reader(packet, kSecurityAdminWireHeaderSize);
    std::uint32_t opcode = 0;
    if (!reader.u32(decoded.header.magic) || !reader.u32(decoded.header.version) ||
        !reader.u32(opcode) || !reader.u32(decoded.header.requestId) ||
        !reader.u32(decoded.header.payloadSize) || !reader.done()) {
        return false;
    }
    decoded.header.opcode = static_cast<SecurityAdminOpcode>(opcode);
    if (decoded.header.magic != kSecurityAdminProtocolMagic ||
        decoded.header.version != kSecurityAdminProtocolVersion ||
        !isValidOpcode(decoded.header.opcode) || decoded.header.requestId == 0 ||
        decoded.header.payloadSize > kSecurityAdminMaxPayload ||
        packetSize != kSecurityAdminWireHeaderSize + decoded.header.payloadSize) {
        return false;
    }
    decoded.payload.assign(packet + kSecurityAdminWireHeaderSize, packet + packetSize);
    return true;
}

bool encodePendingBundleApprovals(const std::vector<PendingBundleApproval>& pending,
                                  std::vector<std::uint8_t>& payload) {
    if (pending.size() > kMaximumPendingRecords) {
        return false;
    }
    Writer writer;
    writer.u32(static_cast<std::uint32_t>(pending.size()));
    for (const PendingBundleApproval& request : pending) {
        if (!isValidPendingApproval(request)) {
            return false;
        }
        writer.u32(static_cast<std::uint32_t>(request.userUid));
        if (!writer.string(request.appId) ||
            !writer.string(request.bundlePath) || !writer.string(request.displayName) ||
            !writer.string(request.appVersion)) {
            return false;
        }
        writer.digest(request.bundleRecordDigest);
        writer.u8(static_cast<std::uint8_t>(request.publisherState));
        writer.u8(static_cast<std::uint8_t>(request.sourceScope));
    }
    payload = writer.take();
    return payload.size() <= kSecurityAdminMaxPayload;
}

bool decodePendingBundleApprovals(const std::vector<std::uint8_t>& payload,
                                  std::vector<PendingBundleApproval>& pending) {
    Reader reader(payload.data(), payload.size());
    std::uint32_t count = 0;
    if (!reader.u32(count) || count > kMaximumPendingRecords) {
        return false;
    }
    pending.clear();
    pending.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        PendingBundleApproval request{};
        std::uint32_t userUid = 0;
        std::uint8_t publisherState = 0;
        std::uint8_t sourceScope = 0;
        if (!reader.u32(userUid) || !reader.string(request.appId) || !reader.string(request.bundlePath) ||
            !reader.string(request.displayName) || !reader.string(request.appVersion) ||
            !reader.digest(request.bundleRecordDigest) || !reader.u8(publisherState) ||
            !reader.u8(sourceScope)) {
            return false;
        }
        request.userUid = static_cast<uid_t>(userUid);
        request.publisherState = static_cast<BundlePublisherState>(publisherState);
        request.sourceScope = static_cast<BundleSourceScope>(sourceScope);
        if (!isValidPendingApproval(request)) {
            return false;
        }
        pending.push_back(std::move(request));
    }
    return reader.done();
}

bool encodeSecurityAdminBundleIdentity(const SecurityAdminBundleIdentity& identity,
                                       std::vector<std::uint8_t>& payload) {
    if (!isValidIdentity(identity)) {
        return false;
    }
    Writer writer;
    if (!writer.string(identity.appId)) {
        return false;
    }
    writer.digest(identity.bundleRecordDigest);
    payload = writer.take();
    return payload.size() <= kSecurityAdminMaxPayload;
}

bool decodeSecurityAdminBundleIdentity(const std::vector<std::uint8_t>& payload,
                                       SecurityAdminBundleIdentity& identity) {
    Reader reader(payload.data(), payload.size());
    if (!reader.string(identity.appId) || !reader.digest(identity.bundleRecordDigest) ||
        !reader.done()) {
        return false;
    }
    return isValidIdentity(identity);
}

bool encodeSecurityAdminApprovalResult(const SecurityAdminApprovalResult& result,
                                       std::vector<std::uint8_t>& payload) {
    if (result.message.empty() || result.message.size() > kMaximumDiagnosticBytes) {
        return false;
    }
    Writer writer;
    writer.u8(result.accepted ? 1 : 0);
    if (!writer.string(result.message)) {
        return false;
    }
    payload = writer.take();
    return payload.size() <= kSecurityAdminMaxPayload;
}

bool decodeSecurityAdminApprovalResult(const std::vector<std::uint8_t>& payload,
                                       SecurityAdminApprovalResult& result) {
    Reader reader(payload.data(), payload.size());
    std::uint8_t accepted = 0;
    if (!reader.u8(accepted) || accepted > 1 || !reader.string(result.message) ||
        result.message.empty() || result.message.size() > kMaximumDiagnosticBytes ||
        !reader.done()) {
        return false;
    }
    result.accepted = accepted == 1;
    return true;
}

bool encodeSecurityAdminError(const std::string& message, std::vector<std::uint8_t>& payload) {
    if (!isVisibleText(message, kMaximumDiagnosticBytes)) {
        return false;
    }
    Writer writer;
    if (!writer.string(message)) {
        return false;
    }
    payload = writer.take();
    return true;
}

bool decodeSecurityAdminError(const std::vector<std::uint8_t>& payload, std::string& message) {
    Reader reader(payload.data(), payload.size());
    return reader.string(message) && isVisibleText(message, kMaximumDiagnosticBytes) && reader.done();
}

} // namespace lcl::security
