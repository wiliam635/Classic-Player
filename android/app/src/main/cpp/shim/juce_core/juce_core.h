#pragma once
// Minimal stand-in for the few juce_core types used by the Nord sample decoders (NordSampleDecoder,
// NordLegacySampleDecoder). The Android app does not link JUCE; this keeps the decoders identical to
// the desktop sources. Only what those files use is provided.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace juce
{
using int64 = long long;
using uint8 = unsigned char;

template <typename T> constexpr T jlimit(T lower, T upper, T value) { return value < lower ? lower : (upper < value ? upper : value); }
template <typename T> constexpr T jmin(T a, T b) { return b < a ? b : a; }
template <typename T> constexpr T jmax(T a, T b) { return a < b ? b : a; }

class String
{
public:
    String() = default;
    String(const char* text) : text_(text != nullptr ? text : "") {}
    String(const std::string& text) : text_(text) {}
    explicit String(int64 value) : text_(std::to_string(value)) {}
    explicit String(int value) : text_(std::to_string(value)) {}
    static String fromUTF8(const char* data, int bytes = -1)
    {
        if (data == nullptr) return {};
        std::size_t length = 0;
        const std::size_t limit = bytes < 0 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(bytes);
        while (length < limit && data[length] != 0) ++length;
        return String(std::string(data, length));
    }
    String trim() const
    {
        const auto first = text_.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = text_.find_last_not_of(" \t\r\n");
        return String(text_.substr(first, last - first + 1));
    }
    bool isEmpty() const { return text_.empty(); }
    bool isNotEmpty() const { return !text_.empty(); }
    const std::string& toStdString() const { return text_; }
    const char* toRawUTF8() const { return text_.c_str(); }
    String& operator+=(const String& other) { text_ += other.text_; return *this; }
    friend String operator+(const String& a, const String& b) { return String(a.text_ + b.text_); }
    friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.text_); }
    friend String operator+(const String& a, const char* b) { return String(a.text_ + b); }
private:
    std::string text_;
};

class Result
{
public:
    static Result ok() { return {}; }
    static Result fail(const String& message) { Result r; r.failed_ = true; r.message_ = message; return r; }
    bool failed() const { return failed_; }
    bool wasOk() const { return !failed_; }
    const String& getErrorMessage() const { return message_; }
private:
    bool failed_ = false;
    String message_;
};

class MemoryBlock
{
public:
    void* getData() { return bytes_.data(); }
    const void* getData() const { return bytes_.data(); }
    std::size_t getSize() const { return bytes_.size(); }
    std::vector<unsigned char>& vector() { return bytes_; }
private:
    std::vector<unsigned char> bytes_;
};

class MemoryMappedFile
{
public:
    const void* getData() const { return nullptr; } // the fast-load cache is not used on Android
};

class File
{
public:
    File() = default;
    explicit File(const std::string& path) : path_(path) {}
    bool existsAsFile() const { std::ifstream in(path_, std::ios::binary); return in.good(); }
    int64 getSize() const
    {
        std::ifstream in(path_, std::ios::binary | std::ios::ate);
        return in.good() ? static_cast<int64>(in.tellg()) : 0;
    }
    bool loadFileAsData(MemoryBlock& destination) const
    {
        std::ifstream in(path_, std::ios::binary | std::ios::ate);
        if (!in.good()) return false;
        const auto size = static_cast<std::size_t>(in.tellg());
        in.seekg(0);
        destination.vector().resize(size);
        in.read(reinterpret_cast<char*>(destination.vector().data()), static_cast<std::streamsize>(size));
        return in.good() || in.eof();
    }
    // `extensions` is one ".ext" or several separated by ';' like JUCE.
    bool hasFileExtension(const char* extensions) const
    {
        const auto dot = path_.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string actual = path_.substr(dot);
        for (auto& c : actual) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string list(extensions), item;
        for (std::size_t i = 0; i <= list.size(); ++i)
        {
            if (i == list.size() || list[i] == ';') { if (item == actual) return true; item.clear(); }
            else item += static_cast<char>(std::tolower(static_cast<unsigned char>(list[i])));
        }
        return false;
    }
    String getFileNameWithoutExtension() const
    {
        const auto slash = path_.find_last_of('/');
        auto name = slash == std::string::npos ? path_ : path_.substr(slash + 1);
        const auto dot = name.find_last_of('.');
        if (dot != std::string::npos) name.resize(dot);
        return String(name);
    }
    String getFullPathName() const { return String(path_); }
private:
    std::string path_;
};

struct SystemStats
{
    static int64 getMemorySizeInMegabytes()
    {
        std::ifstream in("/proc/meminfo");
        std::string key; long long value = 0; std::string unit;
        while (in >> key >> value >> unit)
            if (key == "MemTotal:") return value / 1024;
        return 0;
    }
};
}
