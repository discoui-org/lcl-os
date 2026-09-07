#include "system/security/bundle_signature_backend.hpp"

#include "system/security/app_identity_registry.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <utility>

#if defined(LCL_HAS_OPENSSL_ED25519) && LCL_HAS_OPENSSL_ED25519
#include <openssl/evp.h>
#endif

namespace lcl::security {

bool OpenSslEd25519Verifier::verify(std::span<const std::uint8_t> message,
                                   const Ed25519PublicKey& publicKey,
                                   const Ed25519Signature& signature) const {
#if defined(LCL_HAS_OPENSSL_ED25519) && LCL_HAS_OPENSSL_ED25519
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                publicKey.data(), publicKey.size());
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!key || !context) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return false;
    }
    const bool valid = EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
                       EVP_DigestVerify(context, signature.data(), signature.size(),
                                        message.data(), message.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
#else
    (void)message;
    (void)publicKey;
    (void)signature;
    return false;
#endif
}

RootPublisherTrustStore::RootPublisherTrustStore(RootPublisherTrustStoreConfig config)
    : config_(std::move(config)) {}

std::optional<TrustedPublisher> RootPublisherTrustStore::resolve(
        std::string_view publisherKeyId,
        std::span<const std::uint8_t> certificateChain) const {
    if (!certificateChain.empty()) return std::nullopt;
    const std::string keyId(publisherKeyId);
    if (!AppIdentityRegistry::isValidAppId(keyId)) return std::nullopt;

    struct stat directoryStatus {};
    if (lstat(config_.directory.c_str(), &directoryStatus) != 0 ||
        !S_ISDIR(directoryStatus.st_mode) || S_ISLNK(directoryStatus.st_mode) ||
        directoryStatus.st_uid != config_.ownerUid ||
        directoryStatus.st_gid != config_.ownerGid ||
        (directoryStatus.st_mode & 0022) != 0) {
        return std::nullopt;
    }
    const int directory = open(config_.directory.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return std::nullopt;
    const std::string fileName = keyId + ".ed25519.pub";
    const int descriptor = openat(directory, fileName.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(directory);
    if (descriptor < 0) return std::nullopt;

    struct stat status {};
    Ed25519PublicKey publicKey{};
    std::size_t offset = 0;
    bool valid = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
                 status.st_uid == config_.ownerUid && status.st_gid == config_.ownerGid &&
                 (status.st_mode & 0022) == 0 && status.st_nlink == 1 &&
                 status.st_size == static_cast<off_t>(publicKey.size());
    while (valid && offset < publicKey.size()) {
        const ssize_t count = pread(descriptor, publicKey.data() + offset,
                                    publicKey.size() - offset,
                                    static_cast<off_t>(offset));
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            valid = false;
        }
    }
    struct stat finalStatus {};
    valid = valid && fstat(descriptor, &finalStatus) == 0 &&
            finalStatus.st_dev == status.st_dev && finalStatus.st_ino == status.st_ino &&
            finalStatus.st_size == status.st_size && finalStatus.st_nlink == 1;
    close(descriptor);
    if (!valid) return std::nullopt;

    const std::string_view rawKey(reinterpret_cast<const char*>(publicKey.data()),
                                  publicKey.size());
    return TrustedPublisher{keyId, publicKey, sha256(rawKey)};
}

} // namespace lcl::security
