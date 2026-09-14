#pragma once

#include "system/security/bundle_signature_verifier.hpp"

#include <string>
#include <sys/types.h>

namespace lcl::security {

/** OpenSSL EVP Ed25519 verifier used by canonical Linux userspace binaries. */
class OpenSslEd25519Verifier final : public Ed25519Verifier {
public:
    bool verify(std::span<const std::uint8_t> message,
                const Ed25519PublicKey& publicKey,
                const Ed25519Signature& signature) const override;
};

struct RootPublisherTrustStoreConfig {
    std::string directory{"/System/Library/Security/Publishers"};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/**
 * Resolves directly pinned Ed25519 publisher keys from immutable system data.
 * Each `<publisher-key-id>.ed25519.pub` is exactly 32 raw bytes. Direct pins
 * require an empty envelope certificate chain; unknown or unsafe files fail
 * closed.
 */
class RootPublisherTrustStore final : public PublisherTrustResolver {
public:
    explicit RootPublisherTrustStore(RootPublisherTrustStoreConfig config = {});

    std::optional<TrustedPublisher> resolve(
        std::string_view publisherKeyId,
        std::span<const std::uint8_t> certificateChain) const override;

private:
    RootPublisherTrustStoreConfig config_;
};

} // namespace lcl::security
