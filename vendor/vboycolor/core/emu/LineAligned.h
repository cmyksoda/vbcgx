#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

// An allocator for whole cache lines (32 bytes on the Wii): buffers cleared or written a line at a time.
template <typename T>
struct LineAligned
{
    using value_type = T;
    LineAligned() = default;
    template <typename U>
    LineAligned(const LineAligned<U> &) {}
    T *allocate(size_t n) { return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t(32))); }
    void deallocate(T *p, size_t) { ::operator delete(p, std::align_val_t(32)); }
    template <typename U>
    bool operator==(const LineAligned<U> &) const { return true; }
    template <typename U>
    bool operator!=(const LineAligned<U> &) const { return false; }
};
using PictureBuffer = std::vector<uint8_t, LineAligned<uint8_t>>;

// A 32-byte line cleared (on the Wii claimed in the cache without reading memory first).
inline void ClearLine32(uint8_t *line)
{
#if defined(__PPC__) || defined(__powerpc__)
    __asm__ volatile("dcbz 0,%0" : : "r"(line) : "memory");
#else
    std::memset(line, 0, 32);
#endif
}
