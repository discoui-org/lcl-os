#include "system/security/administrator_protocol.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kMaximumArguments = 64;
constexpr std::size_t kMaximumArgumentBytes = 4096;
constexpr std::size_t kMaximumWorkingDirectoryBytes = 4096;
constexpr std::size_t kMaximumPromptTextBytes = 512;
constexpr std::size_t kMaximumErrorBytes = 1024;
constexpr std::size_t kMaximumOutputBytes = 4096;

class Writer final {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    bool string(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) return false;
        const auto size = static_cast<std::uint16_t>(value.size());
        u8(static_cast<std::uint8_t>(size));
        u8(static_cast<std::uint8_t>(size >> 8));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    void raw(const std::vector<std::uint8_t>& value) {
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
        if (offset_ >= size_) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool string(std::string& value) {
        std::uint8_t low = 0;
        std::uint8_t high = 0;
        if (!u8(low) || !u8(high)) return false;
        const std::size_t length = static_cast<std::size_t>(low) |
                                   (static_cast<std::size_t>(high) << 8);
        if (length > size_ - offset_) return false;
        value.assign(reinterpret_cast<const char*>(bytes_ + offset_), length);
        offset_ += length;
        return true;
    }
    bool remaining(std::vector<std::uint8_t>& value) {
        value.assign(bytes_ + offset_, bytes_ + size_);
        offset_ = size_;
        return true;
    }
    bool done() const noexcept { return offset_ == size_; }

private:
    const std::uint8_t* bytes_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
};

bool visibleText(std::string_view value, std::size_t maximum, bool allowEmpty = false) {
    if ((!allowEmpty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f;
    });
}

bool validOpcode(AdministratorOpcode opcode) {
    return opcode >= AdministratorOpcode::ShellHello &&
           opcode <= AdministratorOpcode::ErrorResponse;
}

bool validExecuteRequest(const AdministratorExecuteRequest& request) {
    if (!visibleText(request.workingDirectory, kMaximumWorkingDirectoryBytes) ||
        request.workingDirectory.front() != '/' || request.arguments.empty() ||
        request.arguments.size() > kMaximumArguments) {
        return false;
    }
    return std::all_of(request.arguments.begin(), request.arguments.end(),
        [](const std::string& argument) {
            return visibleText(argument, kMaximumArgumentBytes, true) &&
                   argument.find('\0') == std::string::npos;
        }) && !request.arguments.front().empty();
}

} // namespace

bool encodeAdministratorPacket(const AdministratorHeader& header,
                               const std::vector<std::uint8_t>& payload,
                               std::vector<std::uint8_t>& packet) {
    if (header.magic != kAdministratorProtocolMagic ||
        header.version != kAdministratorProtocolVersion ||
        !validOpcode(header.opcode) || header.requestId == 0 ||
        header.payloadSize != payload.size() ||
        payload.size() > kAdministratorMaxPayload) {
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

bool decodeAdministratorPacket(const std::uint8_t* packet, std::size_t packetSize,
                               DecodedAdministratorPacket& decoded) {
    if (!packet || packetSize < kAdministratorWireHeaderSize) return false;
    Reader reader(packet, kAdministratorWireHeaderSize);
    std::uint32_t opcode = 0;
    if (!reader.u32(decoded.header.magic) || !reader.u32(decoded.header.version) ||
        !reader.u32(opcode) || !reader.u32(decoded.header.requestId) ||
        !reader.u32(decoded.header.payloadSize) || !reader.done()) {
        return false;
    }
    decoded.header.opcode = static_cast<AdministratorOpcode>(opcode);
    if (decoded.header.magic != kAdministratorProtocolMagic ||
        decoded.header.version != kAdministratorProtocolVersion ||
        !validOpcode(decoded.header.opcode) || decoded.header.requestId == 0 ||
        decoded.header.payloadSize > kAdministratorMaxPayload ||
        packetSize != kAdministratorWireHeaderSize + decoded.header.payloadSize) {
        return false;
    }
    decoded.payload.assign(packet + kAdministratorWireHeaderSize, packet + packetSize);
    return true;
}

bool encodeAdministratorExecuteRequest(const AdministratorExecuteRequest& request,
                                       std::vector<std::uint8_t>& payload) {
    if (!validExecuteRequest(request)) return false;
    Writer writer;
    if (!writer.string(request.workingDirectory)) return false;
    writer.u32(static_cast<std::uint32_t>(request.arguments.size()));
    for (const auto& argument : request.arguments) {
        if (!writer.string(argument)) return false;
    }
    payload = writer.take();
    return payload.size() <= kAdministratorMaxPayload;
}

bool decodeAdministratorExecuteRequest(const std::vector<std::uint8_t>& payload,
                                       AdministratorExecuteRequest& request) {
    Reader reader(payload.data(), payload.size());
    std::uint32_t count = 0;
    if (!reader.string(request.workingDirectory) || !reader.u32(count) ||
        count == 0 || count > kMaximumArguments) {
        return false;
    }
    request.arguments.clear();
    request.arguments.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string argument;
        if (!reader.string(argument)) return false;
        request.arguments.push_back(std::move(argument));
    }
    return reader.done() && validExecuteRequest(request);
}

bool encodeAdministratorPrompt(const AdministratorPrompt& prompt,
                               std::vector<std::uint8_t>& payload) {
    if (!visibleText(prompt.appName, kMaximumPromptTextBytes) ||
        !visibleText(prompt.title, kMaximumPromptTextBytes) ||
        !visibleText(prompt.description, kMaximumPromptTextBytes)) {
        return false;
    }
    Writer writer;
    if (!writer.string(prompt.appName) || !writer.string(prompt.title) ||
        !writer.string(prompt.description)) {
        return false;
    }
    payload = writer.take();
    return true;
}

bool decodeAdministratorPrompt(const std::vector<std::uint8_t>& payload,
                               AdministratorPrompt& prompt) {
    Reader reader(payload.data(), payload.size());
    return reader.string(prompt.appName) && reader.string(prompt.title) &&
           reader.string(prompt.description) && reader.done() &&
           visibleText(prompt.appName, kMaximumPromptTextBytes) &&
           visibleText(prompt.title, kMaximumPromptTextBytes) &&
           visibleText(prompt.description, kMaximumPromptTextBytes);
}

bool encodeAdministratorDecision(bool allowed, std::vector<std::uint8_t>& payload) {
    payload = {static_cast<std::uint8_t>(allowed ? 1 : 0)};
    return true;
}

bool decodeAdministratorDecision(const std::vector<std::uint8_t>& payload, bool& allowed) {
    if (payload.size() != 1 || payload.front() > 1) return false;
    allowed = payload.front() == 1;
    return true;
}

bool encodeAdministratorCommandOutput(const AdministratorCommandOutput& output,
                                      std::vector<std::uint8_t>& payload) {
    if (output.bytes.empty() || output.bytes.size() > kMaximumOutputBytes ||
        (output.stream != AdministratorOutputStream::StandardOutput &&
         output.stream != AdministratorOutputStream::StandardError)) {
        return false;
    }
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(output.stream));
    writer.raw(output.bytes);
    payload = writer.take();
    return true;
}

bool decodeAdministratorCommandOutput(const std::vector<std::uint8_t>& payload,
                                      AdministratorCommandOutput& output) {
    Reader reader(payload.data(), payload.size());
    std::uint8_t stream = 0;
    if (!reader.u8(stream)) return false;
    output.stream = static_cast<AdministratorOutputStream>(stream);
    reader.remaining(output.bytes);
    return !output.bytes.empty() && output.bytes.size() <= kMaximumOutputBytes &&
           (output.stream == AdministratorOutputStream::StandardOutput ||
            output.stream == AdministratorOutputStream::StandardError);
}

bool encodeAdministratorCommandResult(int exitCode, std::vector<std::uint8_t>& payload) {
    if (exitCode < 0 || exitCode > 255) return false;
    payload = {static_cast<std::uint8_t>(exitCode)};
    return true;
}

bool decodeAdministratorCommandResult(const std::vector<std::uint8_t>& payload,
                                      int& exitCode) {
    if (payload.size() != 1) return false;
    exitCode = payload.front();
    return true;
}

bool encodeAdministratorError(const std::string& message,
                              std::vector<std::uint8_t>& payload) {
    if (!visibleText(message, kMaximumErrorBytes)) return false;
    Writer writer;
    if (!writer.string(message)) return false;
    payload = writer.take();
    return true;
}

bool decodeAdministratorError(const std::vector<std::uint8_t>& payload,
                              std::string& message) {
    Reader reader(payload.data(), payload.size());
    return reader.string(message) && reader.done() &&
           visibleText(message, kMaximumErrorBytes);
}

} // namespace lcl::security
