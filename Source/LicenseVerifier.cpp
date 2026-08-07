#include "LicenseVerifier.h"
#include <ClassicPlayerAssets.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>

namespace
{
constexpr auto canonicalPrefix = "CLASSIC-PLAYER|1|PRO|PERPETUAL|";

juce::MemoryBlock decodeBase64Url(juce::String input)
{
    input = input.replaceCharacter('-', '+').replaceCharacter('_', '/');
    while ((input.length() % 4) != 0) input += "=";
    juce::MemoryOutputStream stream;
    if (!juce::Base64::convertFromBase64(stream, input)) return {};
    return stream.getMemoryBlock();
}

juce::MemoryBlock rawSignatureToDer(const juce::MemoryBlock& raw)
{
    if (raw.getSize() != 64) return {};
    auto* bytes = static_cast<const unsigned char*>(raw.getData());
    ECDSA_SIG* signature = ECDSA_SIG_new();
    if (signature == nullptr) return {};
    BIGNUM* r = BN_bin2bn(bytes, 32, nullptr);
    BIGNUM* s = BN_bin2bn(bytes + 32, 32, nullptr);
    if (r == nullptr || s == nullptr || ECDSA_SIG_set0(signature, r, s) != 1)
    {
        BN_free(r); BN_free(s); ECDSA_SIG_free(signature);
        return {};
    }
    const auto length = i2d_ECDSA_SIG(signature, nullptr);
    juce::MemoryBlock der((size_t) length, true);
    auto* destination = static_cast<unsigned char*>(der.getData());
    i2d_ECDSA_SIG(signature, &destination);
    ECDSA_SIG_free(signature);
    return der;
}
}

juce::File LicenseVerifier::licenseFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Classic Keys").getChildFile("Classic Player").getChildFile("license.dat");
}

juce::String LicenseVerifier::storedToken()
{
    const auto file = licenseFile();
    return file.existsAsFile() ? file.loadFileAsString().trim() : juce::String{};
}

bool LicenseVerifier::isActivated()
{
    return verify(storedToken());
}

bool LicenseVerifier::activateAndStore(const juce::String& token)
{
    const auto clean = token.trim();
    if (!verify(clean)) return false;
    auto file = licenseFile();
    if (!file.getParentDirectory().createDirectory()) return false;
    return file.replaceWithText(clean);
}

bool LicenseVerifier::verify(const juce::String& token)
{
    const auto separator = token.indexOfChar('.');
    if (separator <= 0) return false;
    const auto serial = token.substring(0, separator);
    if (!serial.startsWith("CK26-")) return false;

    const auto raw = decodeBase64Url(token.substring(separator + 1));
    const auto der = rawSignatureToDer(raw);
    if (der.getSize() == 0) return false;

    BIO* bio = BIO_new_mem_buf(ClassicPlayerAssets::classicplayerlicensepublic_pem,
                               ClassicPlayerAssets::classicplayerlicensepublic_pemSize);
    if (bio == nullptr) return false;
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr) return false;

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const auto payload = juce::String(canonicalPrefix) + serial;
    bool valid = context != nullptr
        && EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1
        && EVP_DigestVerify(context,
                            static_cast<const unsigned char*>(der.getData()), der.getSize(),
                            reinterpret_cast<const unsigned char*>(payload.toRawUTF8()),
                            (size_t) payload.getNumBytesAsUTF8()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
}
