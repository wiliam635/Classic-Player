#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// 16-bit PCM storage used by the Nord piano / sample decoders.
//
// It looks like a std::vector<int16_t> to the rest of the code, but it can also be a read-only
// view into a memory-mapped fast-load cache file (".cpfc"). A mapped buffer owns no heap memory:
// the operating system pages the audio in from disk on demand and can drop clean pages again
// under memory pressure, so a multi-GB library no longer has to sit in anonymous RAM.
//
// Mapped data is never written to. Decoders and the loop-seam preparation only ever run on owned
// buffers (before the cache file is written); calling a mutating member on a mapped buffer first
// copies it into a private vector (see vec()).
class PcmBuffer
{
public:
    using value_type = std::int16_t;
    using iterator = value_type*;
    using const_iterator = const value_type*;

    std::size_t size() const noexcept { return mapped != nullptr ? mappedCount : owned.size(); }
    bool empty() const noexcept { return size() == 0; }
    // Heap bytes only: a mapped buffer reports 0 because it is not resident anonymous memory.
    std::size_t capacity() const noexcept { return mapped != nullptr ? 0 : owned.capacity(); }
    bool isMapped() const noexcept { return mapped != nullptr; }

    const value_type* data() const noexcept { return mapped != nullptr ? mapped : owned.data(); }
    // Non-const access is read-only for a mapped buffer (writing would fault); owned buffers are writable.
    value_type* data() noexcept { return const_cast<value_type*>(static_cast<const PcmBuffer&>(*this).data()); }

    const value_type& operator[](std::size_t index) const noexcept { return data()[index]; }
    value_type& operator[](std::size_t index) noexcept { return data()[index]; }
    const value_type& back() const noexcept { return data()[size() - 1]; }
    value_type& back() noexcept { return data()[size() - 1]; }

    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + size(); }
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + size(); }

    void reserve(std::size_t count) { vec().reserve(count); }
    void resize(std::size_t count) { vec().resize(count); }
    void push_back(value_type value) { vec().push_back(value); }
    void assign(std::size_t count, value_type value) { vec().assign(count, value); }
    void clear() noexcept { release(); owned.clear(); }

    // Private, writable vector. Copies and un-maps first if this buffer is currently mapped.
    std::vector<value_type>& vec()
    {
        if (mapped != nullptr)
        {
            owned.assign(mapped, mapped + mappedCount);
            release();
        }
        return owned;
    }

    // Point this buffer at `count` samples that start `byteOffset` bytes into a mapped file.
    // The offset must be 2-byte aligned. The mapping stays alive as long as any buffer uses it.
    void mapFrom(std::shared_ptr<juce::MemoryMappedFile> file, std::size_t byteOffset, std::size_t count)
    {
        std::vector<value_type>().swap(owned);
        mappedFile = std::move(file);
        mapped = reinterpret_cast<const value_type*>(static_cast<const char*>(mappedFile->getData()) + byteOffset);
        mappedCount = count;
    }

    bool operator==(const PcmBuffer& other) const noexcept
    {
        return size() == other.size() && std::equal(begin(), end(), other.begin());
    }
    bool operator!=(const PcmBuffer& other) const noexcept { return !(*this == other); }

private:
    void release() noexcept { mapped = nullptr; mappedCount = 0; mappedFile.reset(); }

    std::vector<value_type> owned;
    std::shared_ptr<juce::MemoryMappedFile> mappedFile;
    const value_type* mapped = nullptr;
    std::size_t mappedCount = 0;
};
