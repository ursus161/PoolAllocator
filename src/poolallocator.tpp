#include "poolallocator.hpp"


template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
PoolAllocator<SlotSize, Count, LocalSize, Tag>::PoolAllocator() {
  
    pool = static_cast<Slot*>(std::malloc(sizeof(Slot) * Count));

    if (!pool) [[unlikely]] {
        throw std::bad_alloc{}; //sometimes things go wrong 
    }
   
    for (auto i{0uz}; i < Count - 1; ++i) {
        pool[i].next = &pool[i + 1];
    }
    pool[Count - 1].next = nullptr;

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

    constexpr size_t keep = LocalSize / 2; // flush half of the local pool to global

    auto tail = local_head;
    for(auto i{1uz}; i < keep; ++i) {
        tail = tail->next;
    }

    auto chain = tail->next;
    tail->next = nullptr;

    auto chain_tail = chain;
    while(chain_tail->next != nullptr) {
        chain_tail = chain_tail->next; 
    }

    Slot* old_head = global_head.load(std::memory_order_relaxed);
    do {
        chain_tail->next = old_head;
    } while (!global_head.compare_exchange_weak(old_head, chain, std::memory_order_release, std::memory_order_relaxed));

    local_count = keep;

}

template <size_t SlotSize, size_t Count, size_t LocalSize, typename Tag>
bool PoolAllocator<SlotSize, Count, LocalSize, Tag>::refill_to_local()
{
   Slot* head = global_head.load(std::memory_order_acquire);

    while (head != nullptr) {
        auto* tail = head;
        auto taken{1uz};

        while (taken < LocalSize && tail->next != nullptr) {
            tail = tail->next;
            ++taken;
        }
        
        Slot* rest = tail->next;

        if (global_head.compare_exchange_weak(head, rest,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail->next = nullptr;
            local_head = head;
            local_count = taken;
            return true;
        }
    }
    return false;
}