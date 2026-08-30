#include <gtest/gtest.h>

#include "system/security/bundle_signature_verifier.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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

} // namespace
} // namespace lcl::security
