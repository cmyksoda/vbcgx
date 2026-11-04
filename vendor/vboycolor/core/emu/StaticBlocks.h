#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

// A few buffers of one kind in static storage: on the Wii that's MEM1, where a miss costs about a third of MEM2's, and
// the heap's MEM1 part is full by game time. A buffer too big for a block, or finding every block taken, uses the heap.
template <size_t kBytes, unsigned kCount, typename Kind>
struct StaticBlocks
{
    static_assert(kCount >= 1 && kCount <= 32);
    static constexpr size_t kStride = (kBytes + 63) & ~static_cast<size_t>(63);
    alignas(64) static inline uint8_t storage[kCount][kStride];
    static inline std::atomic<uint32_t> taken{0};

    static void *Take(size_t bytes)
    {
        if (bytes > kStride)
            return nullptr;
        uint32_t was = taken.load(std::memory_order_relaxed);
        for (;;)
        {
            unsigned k = 0;
            while (k < kCount && (was >> k & 1))
                ++k;
            if (k == kCount)
                return nullptr;
            if (taken.compare_exchange_weak(was, was | 1u << k))
                return storage[k];
        }
    }
    static bool Give(void *p)
    {
        const uintptr_t at = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(&storage[0][0]);
        if (at >= sizeof(storage))
            return false;
        taken.fetch_and(~(1u << (at / kStride)));
        return true;
    }
};

// Storage the platform lends for good: on the Wii, the MEM1 the color packs' zip leaves when it moves to MEM2.
struct Loan
{
    static inline uint8_t *next = nullptr, *end = nullptr;

    static void Lend(void *at, size_t bytes)
    {
        end = static_cast<uint8_t *>(at) + bytes;
        next = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(at) + 63) & ~static_cast<uintptr_t>(63));
    }
    static void *Take(size_t bytes)
    {
        if (!next || next > end || static_cast<size_t>(end - next) < bytes)
            return nullptr;
        void *const p = next;
        next += bytes;
        return p;
    }
};

// StaticBlocks in loaned storage, claimed whole by the first buffer of the kind; without a loan, the heap.
template <size_t kBytes, unsigned kCount, typename Kind>
struct LoanBlocks
{
    static_assert(kCount >= 1 && kCount <= 32);
    static constexpr size_t kStride = (kBytes + 63) & ~static_cast<size_t>(63);
    static inline uint8_t *storage = nullptr;
    static inline uint32_t taken = 0;

    static void *Take(size_t bytes)
    {
        if (bytes > kStride)
            return nullptr;
        if (!storage)
            storage = static_cast<uint8_t *>(Loan::Take(kCount * kStride));
        unsigned k = 0;
        while (k < kCount && (taken >> k & 1))
            ++k;
        if (!storage || k == kCount)
            return nullptr;
        taken |= 1u << k;
        return storage + k * kStride;
    }
    static bool Give(void *p)
    {
        const uintptr_t at = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(storage);
        if (!storage || at >= kCount * kStride)
            return false;
        taken &= ~(1u << (at / kStride));
        return true;
    }
};

template <typename T, typename Blocks, size_t kAlign = 32>
struct InStaticBlocks
{
    using value_type = T;
    template <typename U>
    struct rebind
    {
        using other = InStaticBlocks<U, Blocks, kAlign>;
    };
    InStaticBlocks() = default;
    template <typename U>
    InStaticBlocks(const InStaticBlocks<U, Blocks, kAlign> &) {}
    T *allocate(size_t n)
    {
        if (void *p = Blocks::Take(n * sizeof(T)))
            return static_cast<T *>(p);
        return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t(kAlign)));
    }
    void deallocate(T *p, size_t)
    {
        if (!Blocks::Give(p))
            ::operator delete(p, std::align_val_t(kAlign));
    }
    template <typename U>
    bool operator==(const InStaticBlocks<U, Blocks, kAlign> &) const { return true; }
    template <typename U>
    bool operator!=(const InStaticBlocks<U, Blocks, kAlign> &) const { return false; }
};
