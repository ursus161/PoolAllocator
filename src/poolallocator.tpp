#include "poolallocator.hpp"


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
PoolAllocator<SlotSize, Count, LocalSize, Tag>::PoolAllocator() {
   
    constexpr size_t MagCount = Count / LocalSize; // number of magazines in the pool
    pool = static_cast<Slot*>(std::malloc(sizeof(Slot) * Count));

    if (!pool) [[unlikely]] {
        throw std::bad_alloc{}; //sometimes things go wrong 
    }
   
      for (auto m{0uz}; m < MagCount; ++m) {
        Slot* base = &pool[m * LocalSize];

        // sloturile alocabile din interiorul magazinei: s1 -> s2 -> ... -> s31 -> null
        for (auto i{1uz}; i < LocalSize - 1; ++i) {
            base[i].next = &base[i + 1];
        }
        base[LocalSize - 1].next = nullptr; //last

        // capul tine metadata cat timp magazina sta in lista global 
        base[0].mag.next_slot_in_magazine = &base[1]; //first
        base[0].mag.next_magazine = (m + 1 < MagCount)
                             ? &pool[(m + 1) * LocalSize]
                             : nullptr;
    }

    

    global_head.store(pool, std::memory_order_relaxed);
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
    if(local_count >= LocalSize) [[unlikely]] {
         flush_to_global();
    }

}


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
PoolAllocator<SlotSize, Count, LocalSize, Tag>::~PoolAllocator() {
    std::free(pool);
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

    // head carries the metadata: contents start at the second slot
    Slot* first = mag_head->next;
    mag_head->mag.next_slot_in_magazine = first;

    // publish the magazine
    Slot* old_head = global_head.load(std::memory_order_relaxed);
    do {
        mag_head->mag.next_magazine = old_head;
    } while (!global_head.compare_exchange_weak(old_head, mag_head,
                std::memory_order_release, std::memory_order_relaxed));
}


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
bool PoolAllocator<SlotSize, Count, LocalSize, Tag>::refill_to_local() {
    Slot* head = global_head.load(std::memory_order_acquire);

    while (head != nullptr) {
        Slot* next_mag = head->mag.next_magazine;
        Slot* first = head->mag.next_slot_in_magazine;

        if (global_head.compare_exchange_weak(head, next_mag,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            head->next = first;        // capul devine nod obisnuit, metadata nu mai conteaza
            local_head = head;
            local_count = LocalSize;
            return true;
        }
    }
    return false;
}