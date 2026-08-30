#include "system/security/bundle_signature_verifier.hpp"

#include "system/security/app_identity_registry.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::array<std::uint8_t, 8> kEnvelopeMagic = {'L', 'C', 'L', 'S', 'I', 'G', '0', '1'};
constexpr std::uint16_t kEnvelopeVersion = 1;
constexpr std::size_t kMaximumEnvelopeBytes = 64 * 1024;
constexpr std::size_t kMaximumKeyIdBytes = 128;
constexpr std::size_t kMaximumCertificateChainBytes = 48 * 1024;

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (int index = 3; index >= 0; --index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    }
}

bool readU16(std::span<const std::uint8_t> input, std::size_t& offset, std::uint16_t& value) {
    if (offset + 2 > input.size()) {
        return false;
    }
    value = (static_cast<std::uint16_t>(input[offset]) << 8) |
            static_cast<std::uint16_t>(input[offset + 1]);
    offset += 2;
    return true;
}

bool readU32(std::span<const std::uint8_t> input, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > input.size()) {
        return false;
    }
    value = (static_cast<std::uint32_t>(input[offset]) << 24) |
            (static_cast<std::uint32_t>(input[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(input[offset + 2]) << 8) |
            static_cast<std::uint32_t>(input[offset + 3]);
    offset += 4;
    return true;
}

bool validateEnvelopeFields(const BundleSignatureEnvelope& envelope) {
    return AppIdentityRegistry::isValidAppId(envelope.publisherKeyId) &&
           envelope.certificateChain.size() <= kMaximumCertificateChainBytes;
}

std::optional<std::vector<std::uint8_t>> readEnvelopeDescriptor(int descriptor) {
    struct stat status {};
    if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaximumEnvelopeBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = pread(descriptor, contents.data() + offset, contents.size() - offset,
                                    static_cast<off_t>(offset));
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
    struct stat finalStatus {};
    if (fstat(descriptor, &finalStatus) != 0 || finalStatus.st_dev != status.st_dev ||
        finalStatus.st_ino != status.st_ino || finalStatus.st_size != status.st_size ||
        finalStatus.st_nlink != 1) {
        return std::nullopt;
    }
    return contents;
}

BundleSignatureVerification unverified(std::string reason) {
    return {BundlePublisherState::Unverified, {}, {}, std::move(reason)};
}

} // namespace

bool parseBundleSignatureEnvelope(std::span<const std::uint8_t> encoded,
                                  BundleSignatureEnvelope& envelope,
                                  std::string& error) {
    error.clear();
    envelope = {};
    constexpr std::size_t kFixedHeaderBytes = kEnvelopeMagic.size() + 2 + 2 + 4;
    if (encoded.size() < kFixedHeaderBytes + Ed25519Signature{}.size() ||
        encoded.size() > kMaximumEnvelopeBytes ||
        !std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), encoded.begin())) {
        error = "signature envelope has an invalid header";
        return false;
    }

    std::size_t offset = kEnvelopeMagic.size();
    std::uint16_t version = 0;
    std::uint16_t keyIdBytes = 0;
    std::uint32_t certificateChainBytes = 0;
    if (!readU16(encoded, offset, version) || !readU16(encoded, offset, keyIdBytes) ||
        !readU32(encoded, offset, certificateChainBytes) || version != kEnvelopeVersion ||
        keyIdBytes == 0 || keyIdBytes > kMaximumKeyIdBytes ||
        certificateChainBytes > kMaximumCertificateChainBytes) {
        error = "signature envelope has an unsupported version or length";
        return false;
    }
    const std::size_t remaining = encoded.size() - offset;
    if (keyIdBytes > remaining || certificateChainBytes > remaining - keyIdBytes ||
        remaining - keyIdBytes - certificateChainBytes != envelope.signature.size()) {
        error = "signature envelope has inconsistent lengths";
        return false;
    }

    envelope.publisherKeyId.assign(reinterpret_cast<const char*>(encoded.data() + offset), keyIdBytes);
    offset += keyIdBytes;
    envelope.certificateChain.assign(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                                     encoded.begin() + static_cast<std::ptrdiff_t>(offset + certificateChainBytes));
    offset += certificateChainBytes;
    std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(offset), envelope.signature.size(),
                envelope.signature.begin());
    if (!validateEnvelopeFields(envelope)) {
        error = "signature envelope has an invalid publisher key ID or certificate chain";
        envelope = {};
        return false;
    }
    return true;
}

std::vector<std::uint8_t> serializeBundleSignatureEnvelope(const BundleSignatureEnvelope& envelope) {
    if (!validateEnvelopeFields(envelope) || envelope.publisherKeyId.size() > kMaximumKeyIdBytes) {
        return {};
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(kEnvelopeMagic.size() + 2 + 2 + 4 + envelope.publisherKeyId.size() +
                    envelope.certificateChain.size() + envelope.signature.size());
    encoded.insert(encoded.end(), kEnvelopeMagic.begin(), kEnvelopeMagic.end());
    appendU16(encoded, kEnvelopeVersion);
    appendU16(encoded, static_cast<std::uint16_t>(envelope.publisherKeyId.size()));
    appendU32(encoded, static_cast<std::uint32_t>(envelope.certificateChain.size()));
    encoded.insert(encoded.end(), envelope.publisherKeyId.begin(), envelope.publisherKeyId.end());
    encoded.insert(encoded.end(), envelope.certificateChain.begin(), envelope.certificateChain.end());
    encoded.insert(encoded.end(), envelope.signature.begin(), envelope.signature.end());
    return encoded;
}

BundleSignatureVerification BundleSignatureVerifier::verify(
    const lcl::core::AppBundleMetadata& metadata, const BundleRecord& record) const {
    std::string error;
    if (!validateBundleRecord(record, error)) {
        return unverified("bundle record is invalid: " + error);
    }
    if (!metadata.signatureHandle || !metadata.signatureHandle->valid() ||
        isZeroDigest(record.signatureEnvelopeDigest)) {
        return unverified("bundle has no verified detached signature envelope");
    }
    const auto envelopeBytes = readEnvelopeDescriptor(metadata.signatureHandle->descriptor());
    if (!envelopeBytes || sha256(std::string_view(
                              reinterpret_cast<const char*>(envelopeBytes->data()), envelopeBytes->size())) !=
                              record.signatureEnvelopeDigest) {
        return unverified("signature envelope changed after bundle record verification");
    }

    BundleSignatureEnvelope envelope;
    if (!parseBundleSignatureEnvelope(
            std::span<const std::uint8_t>(envelopeBytes->data(), envelopeBytes->size()),
            envelope, error)) {
        return unverified("signature envelope is malformed: " + error);
    }
    const auto publisher = trustResolver_.resolve(
        envelope.publisherKeyId,
        std::span<const std::uint8_t>(envelope.certificateChain.data(), envelope.certificateChain.size()));
    if (!publisher || publisher->publisherKeyId != envelope.publisherKeyId ||
        isZeroDigest(publisher->fingerprint)) {
        return unverified("publisher key or certificate chain is not trusted");
    }

    const std::string payload = serializeBundleRecordPayload(record);
    const auto message = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    if (!ed25519_.verify(message, publisher->publicKey, envelope.signature)) {
        return unverified("Ed25519 signature verification failed");
    }
    return {BundlePublisherState::SignatureVerified, envelope.publisherKeyId,
            publisher->fingerprint, "publisher signature verified"};
}

} // namespace lcl::security
