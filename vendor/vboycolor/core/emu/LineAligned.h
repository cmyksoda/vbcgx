#pragma once

#include <cstddef>
#include <cstdint>
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
