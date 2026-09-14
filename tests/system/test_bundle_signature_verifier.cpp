#include <gtest/gtest.h>

#include "system/security/bundle_signature_verifier.hpp"
#include "system/security/bundle_signature_backend.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class FakeEd25519Verifier final : public Ed25519Verifier {
public:
    bool verify(std::span<const std::uint8_t> message, const Ed25519PublicKey& publicKey,
                const Ed25519Signature& signature) const override {
        called = true;
        return !message.empty() && publicKey[0] == 0x42 && signature[0] == 0xA5;
    }

    mutable bool called{false};
};

class FakePublisherTrustResolver final : public PublisherTrustResolver {
public:
    std::optional<TrustedPublisher> resolve(
        std::string_view publisherKeyId,
        std::span<const std::uint8_t> certificateChain) const override {
        if (publisherKeyId != "org.lcl.publisher" ||
            certificateChain.size() != 2 || certificateChain[0] != 0x10 ||
            certificateChain[1] != 0x20) {
            return std::nullopt;
        }
        TrustedPublisher publisher{};
        publisher.publisherKeyId = "org.lcl.publisher";
        publisher.publicKey[0] = 0x42;
        publisher.fingerprint = sha256("publisher-fingerprint");
        return publisher;
    }
};

class BundleSignatureVerifierTest : public ::testing::Test {
protected:
    fs::path temporaryDirectory_;

    void SetUp() override {
        temporaryDirectory_ = fs::temp_directory_path() /
                              ("lcl_bundle_signature_" + std::to_string(getpid()));
        fs::remove_all(temporaryDirectory_);
        fs::create_directories(temporaryDirectory_ / "Example.app" / "Resources");
        fs::create_directories(temporaryDirectory_ / "Example.app" / "Executables");
        chmod(temporaryDirectory_.c_str(), 0700);

        std::ofstream manifest(bundlePath() / "Manifest.json");
        manifest << R"({"id":"org.lcl.example","name":"Example","version":"1.0","icon":"Resources/Icon.png","executable":"Executables/app"})";
        manifest.close();
        std::ofstream icon(bundlePath() / "Resources" / "Icon.png");
        icon << "icon";
        icon.close();
        std::ofstream executable(bundlePath() / "Executables" / "app");
        executable << "#!/bin/lcl\n";
        executable.close();
        chmod((bundlePath() / "Executables" / "app").c_str(), 0755);
    }

    void TearDown() override {
        fs::remove_all(temporaryDirectory_);
    }

    fs::path bundlePath() const { return temporaryDirectory_ / "Example.app"; }

    void writeEnvelope(std::uint8_t signatureFirstByte = 0xA5) const {
        BundleSignatureEnvelope envelope{};
        envelope.publisherKeyId = "org.lcl.publisher";
        envelope.certificateChain = {0x10, 0x20};
        envelope.signature[0] = signatureFirstByte;
        const std::vector<std::uint8_t> encoded = serializeBundleSignatureEnvelope(envelope);
        ASSERT_FALSE(encoded.empty());
        std::ofstream signature(bundlePath() / "Signature.ed25519", std::ios::binary | std::ios::trunc);
        signature.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    std::pair<lcl::core::AppBundleMetadata, BundleRecord> parseAndRecord() const {
        const auto metadata = lcl::core::AppBundleParser::parseBundle(bundlePath().string());
        EXPECT_TRUE(metadata.has_value());
        std::string error;
        std::optional<BundleRecord> record;
        if (metadata) {
            record = makeBundleRecord(*metadata, error);
        }
        EXPECT_TRUE(record.has_value()) << error;
        return {metadata.value_or(lcl::core::AppBundleMetadata{}), record.value_or(BundleRecord{})};
    }
};

TEST_F(BundleSignatureVerifierTest, VerifiesTrustedDescriptorHeldEnvelopeAgainstPayload) {
    writeEnvelope();
    auto [metadata, record] = parseAndRecord();
    ASSERT_TRUE(metadata.signatureHandle);

    FakeEd25519Verifier backend;
    FakePublisherTrustResolver trustResolver;
    BundleSignatureVerifier verifier(backend, trustResolver);
    const BundleSignatureVerification result = verifier.verify(metadata, record);

    EXPECT_EQ(result.publisherState, BundlePublisherState::SignatureVerified);
    EXPECT_EQ(result.publisherKeyId, "org.lcl.publisher");
    EXPECT_EQ(result.publisherFingerprint, sha256("publisher-fingerprint"));
    EXPECT_TRUE(backend.called);
}

TEST_F(BundleSignatureVerifierTest, RejectsMalformedAndUnknownPublisherEnvelopes) {
    std::ofstream malformed(bundlePath() / "Signature.ed25519", std::ios::binary);
    malformed << "not an LCL signature envelope";
    malformed.close();
    auto [metadata, record] = parseAndRecord();

    FakeEd25519Verifier backend;
    FakePublisherTrustResolver trustResolver;
    BundleSignatureVerifier verifier(backend, trustResolver);
    const BundleSignatureVerification malformedResult = verifier.verify(metadata, record);
    EXPECT_EQ(malformedResult.publisherState, BundlePublisherState::Unverified);
    EXPECT_FALSE(backend.called);

    writeEnvelope();
    auto [knownMetadata, knownRecord] = parseAndRecord();
    BundleSignatureEnvelope unknownEnvelope{};
    unknownEnvelope.publisherKeyId = "org.lcl.unknown";
    unknownEnvelope.signature[0] = 0xA5;
    const std::vector<std::uint8_t> unknownBytes = serializeBundleSignatureEnvelope(unknownEnvelope);
    ASSERT_FALSE(unknownBytes.empty());
    std::ofstream unknown(bundlePath() / "Signature.ed25519", std::ios::binary | std::ios::trunc);
    unknown.write(reinterpret_cast<const char*>(unknownBytes.data()),
                  static_cast<std::streamsize>(unknownBytes.size()));
    unknown.close();

    const BundleSignatureVerification changedResult = verifier.verify(knownMetadata, knownRecord);
    EXPECT_EQ(changedResult.publisherState, BundlePublisherState::Unverified);
    EXPECT_NE(changedResult.reason.find("changed"), std::string::npos);
}

TEST_F(BundleSignatureVerifierTest, RejectsInvalidEnvelopeSerialization) {
    BundleSignatureEnvelope invalid{};
    invalid.publisherKeyId = "Publisher.Not.Lowercase";
    EXPECT_TRUE(serializeBundleSignatureEnvelope(invalid).empty());

    BundleSignatureEnvelope parsed{};
    std::string error;
    const std::array<std::uint8_t, 3> truncated = {0, 1, 2};
    EXPECT_FALSE(parseBundleSignatureEnvelope(
        std::span<const std::uint8_t>(truncated.data(), truncated.size()), parsed, error));
    EXPECT_FALSE(error.empty());
}

std::vector<std::uint8_t> decodeHex(std::string_view input) {
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        return 0xff;
    };
    std::vector<std::uint8_t> result;
    if (input.size() % 2 != 0) return result;
    result.reserve(input.size() / 2);
    for (std::size_t index = 0; index < input.size(); index += 2) {
        const std::uint8_t high = nibble(input[index]);
        const std::uint8_t low = nibble(input[index + 1]);
        if (high == 0xff || low == 0xff) return {};
        result.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return result;
}

TEST_F(BundleSignatureVerifierTest, OpenSslBackendAcceptsRfc8032VectorAndRejectsMutation) {
    const auto publicBytes = decodeHex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const auto signatureBytes = decodeHex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    ASSERT_EQ(publicBytes.size(), Ed25519PublicKey{}.size());
    ASSERT_EQ(signatureBytes.size(), Ed25519Signature{}.size());
    Ed25519PublicKey publicKey{};
    Ed25519Signature signature{};
    std::copy(publicBytes.begin(), publicBytes.end(), publicKey.begin());
    std::copy(signatureBytes.begin(), signatureBytes.end(), signature.begin());

    OpenSslEd25519Verifier verifier;
    const std::array<std::uint8_t, 0> emptyMessage{};
    EXPECT_TRUE(verifier.verify(emptyMessage, publicKey, signature));
    signature[0] ^= 1;
    EXPECT_FALSE(verifier.verify(emptyMessage, publicKey, signature));
}

TEST_F(BundleSignatureVerifierTest, RootTrustStoreAcceptsOnlyPinnedSafeRawKey) {
    const fs::path trustDirectory = temporaryDirectory_ / "Publishers";
    fs::create_directory(trustDirectory);
    ASSERT_EQ(chmod(trustDirectory.c_str(), 0700), 0);
    Ed25519PublicKey publicKey{};
    publicKey[0] = 0x42;
    const fs::path keyPath = trustDirectory / "org.lcl.publisher.ed25519.pub";
    std::ofstream key(keyPath, std::ios::binary);
    key.write(reinterpret_cast<const char*>(publicKey.data()), publicKey.size());
    key.close();
    ASSERT_EQ(chmod(keyPath.c_str(), 0600), 0);

    RootPublisherTrustStore trust({trustDirectory.string(), getuid(), getgid()});
    const auto publisher = trust.resolve("org.lcl.publisher", {});
    ASSERT_TRUE(publisher);
    EXPECT_EQ(publisher->publicKey, publicKey);
    EXPECT_EQ(publisher->fingerprint,
              sha256(std::string_view(reinterpret_cast<const char*>(publicKey.data()),
                                      publicKey.size())));
    const std::array<std::uint8_t, 1> untrustedChain{1};
    EXPECT_FALSE(trust.resolve("org.lcl.publisher", untrustedChain));
    EXPECT_FALSE(trust.resolve("org.lcl.unknown", {}));
}

} // namespace
} // namespace lcl::security
