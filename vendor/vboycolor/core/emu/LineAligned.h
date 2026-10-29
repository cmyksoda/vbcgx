#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

// An allocator for whole cache lines (32 bytes on the Wii): buffers cleared or written a line at a time.
template <typename T, size_t kAlign = 32>
struct LineAligned
{
    using value_type = T;
    template <typename U>
    struct rebind
    {
        using other = LineAligned<U, kAlign>;
    };
    LineAligned() = default;
    template <typename U>
    LineAligned(const LineAligned<U, kAlign> &) {}
    T *allocate(size_t n) { return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t(kAlign))); }
    void deallocate(T *p, size_t) { ::operator delete(p, std::align_val_t(kAlign)); }
    template <typename U>
    bool operator==(const LineAligned<U, kAlign> &) const { return true; }
    template <typename U>
    bool operator!=(const LineAligned<U, kAlign> &) const { return false; }
};
// (depth's buffers: each starts a 64-byte L2 line, so malloc's 8/16-byte phases can't change what shares a line)
template <typename T>
using Vec64 = std::vector<T, LineAligned<T, 64>>;
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
