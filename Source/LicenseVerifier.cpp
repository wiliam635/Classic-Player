#include "LicenseVerifier.h"
#include <ClassicPlayerAssets.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>
#include <atomic>

namespace
{
constexpr auto canonicalPrefix = "CLASSIC-PLAYER|1|PRO|PERPETUAL|";
constexpr auto licenseServiceUrl = "https://licenca.classickeys.com.br";
std::atomic<bool> onlineSessionValidated { false };

juce::File sessionFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Classic Keys").getChildFile("Classic Player").getChildFile("session.dat");
}

juce::String deviceId()
{
    auto id = juce::SystemStats::getUniqueDeviceID().trim();
    if (id.length() < 16)
        id = "classic-player|" + juce::SystemStats::getComputerName() + "|"
             + juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getFullPathName();
    return id;
}

juce::String platformName()
{
   #if JUCE_MAC
    return "macOS";
   #elif JUCE_WINDOWS
    return "Windows";
   #else
    return "Desktop";
   #endif
}

bool postJson(const juce::String& path, const juce::var& payload,
              juce::var& response, int& status, juce::String& errorMessage,
              const juce::String& bearerToken = {})
{
    auto json = juce::JSON::toString(payload);
    auto url = juce::URL(juce::String(licenseServiceUrl) + path).withPOSTData(json);
    juce::StringPairArray headers;
    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
        .withExtraHeaders("Content-Type: application/json\r\nAccept: application/json"
                          + (bearerToken.isNotEmpty() ? "\r\nAuthorization: Bearer " + bearerToken : juce::String{}))
        .withConnectionTimeoutMs(10000)
        .withNumRedirectsToFollow(3)
        .withStatusCode(&status)
        .withResponseHeaders(&headers);
    auto stream = url.createInputStream(options);
    if (stream == nullptr)
    {
        errorMessage = "Não foi possível conectar ao servidor de licença.";
        return false;
    }
    response = juce::JSON::parse(stream->readEntireStreamAsString());
    if (status < 200 || status >= 300)
    {
        auto serverError = response.getProperty("error", {}).toString();
        errorMessage = serverError.isNotEmpty() ? serverError
                                                : "O servidor recusou a autenticação.";
        return false;
    }
    return true;
}

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
    // Desktop releases protected by the online account service must never
    // bypass the login screen because of a legacy offline activation file.
    // A persisted online session is only accepted after validateOnlineSession()
    // confirms it with the licensing server in this process.
    return onlineSessionValidated.load(std::memory_order_acquire);
}

bool LicenseVerifier::activateAndStore(const juce::String& token)
{
    const auto clean = token.trim();
    if (!verify(clean)) return false;
    auto file = licenseFile();
    if (!file.getParentDirectory().createDirectory()) return false;
    return file.replaceWithText(clean);
}

bool LicenseVerifier::hasOnlineSession()
{
    return sessionFile().existsAsFile() && sessionFile().loadFileAsString().trim().length() >= 24;
}

void LicenseVerifier::clearOnlineSession()
{
    sessionFile().deleteFile();
    onlineSessionValidated.store(false, std::memory_order_release);
}

bool LicenseVerifier::loginOnline(const juce::String& email, const juce::String& password,
                                  juce::String& errorMessage)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("email", email.trim().toLowerCase());
    object->setProperty("password", password);
    object->setProperty("device_id", deviceId());
    object->setProperty("platform", platformName());
    object->setProperty("device_name", juce::SystemStats::getComputerName());
    juce::var response;
    int status = 0;
    if (!postJson("/v1/auth/login", juce::var(object), response, status, errorMessage)) return false;
    const auto token = response.getProperty("access_token", {}).toString().trim();
    if (token.length() < 24) { errorMessage = "Resposta de licença inválida."; return false; }
    auto file = sessionFile();
    if (file.getParentDirectory().createDirectory().failed() || !file.replaceWithText(token))
    { errorMessage = "Não foi possível salvar a licença neste computador."; return false; }
    onlineSessionValidated.store(true, std::memory_order_release);
    return true;
}

bool LicenseVerifier::validateOnlineSession(juce::String& errorMessage)
{
    const auto token = sessionFile().loadFileAsString().trim();
    if (token.length() < 24) { errorMessage = "Sessão de licença ausente."; return false; }
    juce::var response; int status = 0;
    auto* object = new juce::DynamicObject();
    if (!postJson("/v1/license/validate", juce::var(object), response, status, errorMessage, token))
    { onlineSessionValidated.store(false, std::memory_order_release); return false; }
    const auto valid = static_cast<bool>(response.getProperty("valid", false));
    onlineSessionValidated.store(valid, std::memory_order_release);
    return valid;
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
