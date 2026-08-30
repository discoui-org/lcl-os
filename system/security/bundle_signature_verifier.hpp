#pragma once

#include "system/security/bundle_record.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lcl::core {
struct AppBundleMetadata;
}

namespace lcl::security {

using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

/**
 * Detached Signature.ed25519 envelope, version 1:
 *   magic[8] = "LCLSIG01", u16 version, u16 key-id bytes,
 *   u32 certificate-chain bytes, key-id, certificate chain, signature[64].
 *
 * The signature is over serializeBundleRecordPayload(record), never over the
 * envelope itself. The certificate chain is opaque to this parser and is
 * validated by the root-owned trust resolver.
 */
struct BundleSignatureEnvelope {
    std::string publisherKeyId;
    std::vector<std::uint8_t> certificateChain;
    Ed25519Signature signature{};
};

bool parseBundleSignatureEnvelope(std::span<const std::uint8_t> encoded,
                                  BundleSignatureEnvelope& envelope,
                                  std::string& error);
std::vector<std::uint8_t> serializeBundleSignatureEnvelope(
    const BundleSignatureEnvelope& envelope);

/** Crypto backend supplied by one audited, same-binary LCL dependency. */
class Ed25519Verifier {
public:
    virtual ~Ed25519Verifier() = default;
    virtual bool verify(std::span<const std::uint8_t> message,
                        const Ed25519PublicKey& publicKey,
                        const Ed25519Signature& signature) const = 0;
};

struct TrustedPublisher {
    std::string publisherKeyId;
    Ed25519PublicKey publicKey{};
    Sha256Digest fingerprint{};
};

/** Resolves and validates the envelope's certificate chain from a trusted store. */
class PublisherTrustResolver {
public:
    virtual ~PublisherTrustResolver() = default;
    virtual std::optional<TrustedPublisher> resolve(
        std::string_view publisherKeyId,
        std::span<const std::uint8_t> certificateChain) const = 0;
};

struct BundleSignatureVerification {
    BundlePublisherState publisherState{BundlePublisherState::Unverified};
    std::string publisherKeyId;
    Sha256Digest publisherFingerprint{};
    std::string reason;
};

/**
 * Reads the parser-held signature descriptor and verifies it against the
 * exact payload of a canonical BundleRecord. No absent, malformed, unknown or
 * backend-failed signature can result in SignatureVerified.
 */
class BundleSignatureVerifier final {
public:
    BundleSignatureVerifier(const Ed25519Verifier& ed25519,
                            const PublisherTrustResolver& trustResolver)
        : ed25519_(ed25519), trustResolver_(trustResolver) {}

    BundleSignatureVerification verify(const lcl::core::AppBundleMetadata& metadata,
                                       const BundleRecord& record) const;

private:
    const Ed25519Verifier& ed25519_;
    const PublisherTrustResolver& trustResolver_;
};

} // namespace lcl::security
