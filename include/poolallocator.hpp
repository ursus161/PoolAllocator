#pragma once
#include <new>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cassert>

#ifdef __cpp_lib_hardware_interference_size
    inline static constexpr size_t CacheLine = std::hardware_destructive_interference_size;
#elif defined(__APPLE__) && defined(__aarch64__)
    inline static constexpr size_t CacheLine = 128; // Apple M usual cache line size
#else
    inline static constexpr size_t CacheLine = 64; // assume a fallback (shouldnt happen)
#endif


template <size_t SlotSize, size_t Count, size_t LocalSize = 32, typename Tag = void>
class PoolAllocator {

    static_assert(SlotSize > 0, "SlotSize must be greater than 0");
    static_assert(Count > 0, "Count must be greater than 0");
    static_assert(LocalSize > 0, "LocalSize must be greater than 0");
    static_assert(LocalSize <= Count, "LocalSize must be less than or equal to Count");
    static_assert((LocalSize & (LocalSize - 1)) == 0, "LocalSize must be a power of 2");
    static_assert(SlotSize >= sizeof(void*), "SlotSize must be greater than or equal to sizeof(void*)");
    static_assert((SlotSize & (SlotSize - 1)) == 0, "SlotSize must be a power of 2");
    
    union Slot {
        Slot* next;
        char data[SlotSize];
    };

    static thread_local Slot* local_head;
    static thread_local size_t local_count;


    alignas(CacheLine) std::atomic<Slot*> global_head;
    alignas(CacheLine) Slot* pool;

public:
    PoolAllocator();
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) noexcept = delete; //dangling ptrs danger 
    PoolAllocator& operator=(PoolAllocator&&) noexcept = delete;

    void* allocate() noexcept;
    void deallocate(void* ptr) noexcept;
    
    void flush_to_global();
    bool refill_to_local(); // false if global pool is empty

    static size_t get_local_count()   {
        return local_count;
    }
    
};




template <size_t S, size_t C, size_t L, typename Tag>
thread_local typename PoolAllocator<S, C, L, Tag>::Slot*
    PoolAllocator<S, C, L, Tag>::local_head = nullptr;

template <size_t S, size_t C, size_t L, typename Tag>
thread_local size_t PoolAllocator<S, C, L, Tag>::local_count = 0;
#include "../src/poolallocator.tpp"