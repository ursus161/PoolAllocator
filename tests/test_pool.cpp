#include "poolallocator.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) ++failures;
}

//single threaded 

static void test_uniqueness() {
    PoolAllocator<64, 1024> pool;
    std::unordered_set<void*> seen;
    bool ok = true;

    for (size_t i = 0; i < 1024; ++i) {
        void* p = pool.allocate();
        if (!p || !seen.insert(p).second) { ok = false; break; }
    }
    check(ok && seen.size() == 1024, "each slot is returned exactly once");
}

static void test_pattern() {
    PoolAllocator<64, 512> pool;
    std::vector<uint64_t*> ptrs;

    for (size_t i = 0; i < 512; ++i) {
        auto* p = static_cast<uint64_t*>(pool.allocate());
        if (!p) break;
        *p = 0xA5A5'0000ull + i;
        ptrs.push_back(p);
    }

    bool ok = ptrs.size() == 512;
    for (size_t i = 0; i < ptrs.size(); ++i)
        if (*ptrs[i] != 0xA5A5'0000ull + i) { ok = false; break; }

    check(ok, "slots do not overwrite each other");
}

static void test_exhaustion() {
    PoolAllocator<64, 256> pool;
    size_t got = 0;
    while (pool.allocate() != nullptr) {
        if (++got > 256) break;
    }
    check(got == 256, "exhausted pool returns nullptr, no crash");
}

static void test_reuse() {
    PoolAllocator<64, 256> pool;
    std::vector<void*> ptrs;

    for (size_t i = 0; i < 256; ++i) {
        void* p = pool.allocate();
        if (!p) break;
        ptrs.push_back(p);
    }
    for (void* p : ptrs) pool.deallocate(p);

    size_t again = 0;
    while (pool.allocate() != nullptr) {
        if (++again > 256) break;
    }
    check(ptrs.size() == 256 && again == 256, "pool fully recovers after free");
}

//multi-thread 
static void test_concurrent() {
    constexpr size_t THREADS = 8;
    constexpr size_t OPS = 50'000;
    constexpr size_t HELD = 32;

    PoolAllocator<64, 1 << 16> pool;
    std::atomic<size_t> failed_allocs{0};
    std::atomic<size_t> corrupted{0};

    auto worker = [&](size_t id) {
        std::mt19937 rng(static_cast<unsigned>(id * 7919 + 13));
        std::vector<uint64_t*> held;
        held.reserve(HELD);

        for (size_t i = 0; i < OPS; ++i) {
            bool do_alloc = held.size() < HELD &&
                            (held.empty() || (rng() & 1));

            if (do_alloc) {
                auto* p = static_cast<uint64_t*>(pool.allocate());
                if (!p) { ++failed_allocs; continue; }
                *p = 0xC0FFEE00ull + id;      // mark which thread owns the slot
                held.push_back(p);
            } else {
                size_t k = rng() % held.size();
                if (*held[k] != 0xC0FFEE00ull + id) ++corrupted;
                pool.deallocate(held[k]);
                held[k] = held.back();
                held.pop_back();
            }
        }
        for (auto* p : held) pool.deallocate(p);
    };

    std::vector<std::thread> ts;
    for (size_t t = 0; t < THREADS; ++t) ts.emplace_back(worker, t);
    for (auto& t : ts) t.join();

    std::printf("       (%zu failed allocations, %zu corrupted slots)\n",
                failed_allocs.load(), corrupted.load());
    check(corrupted == 0, "no slot is held by two threads at the same time");
}
 

static void test_contention() {
    constexpr size_t THREADS = 8;
    constexpr size_t OPS = 20'000;
    constexpr size_t BURST = 64;    

    PoolAllocator<64, 1 << 16> pool;
    std::atomic<size_t> corrupted{0};

    auto worker = [&](size_t id) {
        std::vector<uint64_t*> burst(BURST);
        for (size_t i = 0; i < OPS; ++i) {
            for (size_t j = 0; j < BURST; ++j) {
                burst[j] = static_cast<uint64_t*>(pool.allocate());
                if (burst[j]) *burst[j] = 0xDEAD0000ull + id * BURST + j;
            }
            for (size_t j = 0; j < BURST; ++j) {
                if (!burst[j]) continue;
                if (*burst[j] != 0xDEAD0000ull + id * BURST + j) ++corrupted;
                pool.deallocate(burst[j]);
            }
        }
    };

    std::vector<std::thread> ts;
    for (size_t t = 0; t < THREADS; ++t) ts.emplace_back(worker, t);
    for (auto& t : ts) t.join();

    check(corrupted == 0, "refill/flush under contention does not corrupt data");
}

int main() {
    test_uniqueness();
    test_pattern();
    test_exhaustion();
    test_reuse();
    test_concurrent();
    test_contention();

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}