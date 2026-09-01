#include "poolallocator.hpp"


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
PoolAllocator<SlotSize, Count, LocalSize, Tag>::PoolAllocator() {

    constexpr size_t MagCount = Count / LocalSize; // number of magazines in the pool

    mag_next = new std::atomic<uint32_t>[Count]; // throws bad_alloc on its own
    pool = static_cast<Slot*>(std::malloc(sizeof(Slot) * Count));

    if (!pool) [[unlikely]] {
        delete[] mag_next;
        throw std::bad_alloc{}; //sometimes things go wrong
    }

    for (auto m{0uz}; m < MagCount; ++m) {
        Slot* base = &pool[m * LocalSize];

        // every slot of the magazine is a plain list node: s0 -> s1 -> ... -> s31 -> null
        for (auto i{0uz}; i < LocalSize - 1; ++i) {
            base[i].next = &base[i + 1];
        }
        base[LocalSize - 1].next = nullptr; //last

        // the magazine link lives in the side array, keyed by the head slot index
        mag_next[m * LocalSize].store(
            (m + 1 < MagCount) ? static_cast<uint32_t>((m + 1) * LocalSize) : NIL,
            std::memory_order_relaxed);
    }

    global_head.store(pack(0, 0), std::memory_order_relaxed);
}

template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
void* PoolAllocator<SlotSize, Count, LocalSize, Tag>::allocate() noexcept {
    if(local_head == nullptr) [[unlikely]]{
        if(!refill_to_local()) [[unlikely]]{
            return nullptr; // global pool is empty
        }
    }
    auto* slot = local_head;
    local_head = slot->next;
    --local_count;
    return slot;
}

template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
void PoolAllocator<SlotSize, Count, LocalSize, Tag>::deallocate(void* ptr) noexcept
{   
    if (ptr == nullptr) [[unlikely]] return;

    assert(ptr >= pool && ptr < pool + Count); //on testing only assertion, removed in prod run
    assert((reinterpret_cast<char*>(ptr) - reinterpret_cast<char*>(pool)) % sizeof(Slot) == 0);

    
    auto* slot = static_cast<Slot*>(ptr);
    slot->next = local_head;
    local_head = slot;
    ++local_count;
    if(local_count >= 2 * LocalSize) [[unlikely]] {
         flush_to_global();
    }

}


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
PoolAllocator<SlotSize, Count, LocalSize, Tag>::~PoolAllocator() {
    std::free(pool);
    delete[] mag_next;
}


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
void PoolAllocator<SlotSize, Count, LocalSize, Tag>::flush_to_global() {
    // the head of the local chain becomes the head of the new magazine
    Slot* mag_head = local_head;

   //  find the last slot of the batch
    Slot* tail = local_head;
    for (auto i{1uz}; i < LocalSize; ++i) {
        tail = tail->next;
    }

    // what stays local
    local_head = tail->next;
    local_count -= LocalSize;

    // close the magazine's internal chain
    tail->next = nullptr;

    const auto idx = static_cast<uint32_t>(mag_head - pool);

    // publish the magazine; the link goes to the side array, never into a slot
    uint64_t old_head = global_head.load(std::memory_order_relaxed);
    uint64_t new_head;
    do {
        mag_next[idx].store(static_cast<uint32_t>(old_head), std::memory_order_relaxed);
        new_head = pack(static_cast<uint32_t>(old_head >> 32) + 1, idx);
    } while (!global_head.compare_exchange_weak(old_head, new_head,
                std::memory_order_release, std::memory_order_relaxed));
}


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
bool PoolAllocator<SlotSize, Count, LocalSize, Tag>::refill_to_local() {
    uint64_t head = global_head.load(std::memory_order_acquire);

    while (static_cast<uint32_t>(head) != NIL) {
        const auto idx = static_cast<uint32_t>(head);

        // speculative: may be stale if we lose the CAS, but it only ever reads the
        // side array, so it can never follow user data written into a taken slot
        const uint64_t new_head = pack(static_cast<uint32_t>(head >> 32) + 1,
                                       mag_next[idx].load(std::memory_order_relaxed));

        if (global_head.compare_exchange_weak(head, new_head,
                std::memory_order_acquire, std::memory_order_acquire)) {
            local_head = &pool[idx];   // the whole magazine is ours now
            local_count = LocalSize;
            return true;
        }
    }
    return false;
}
