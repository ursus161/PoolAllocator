#include "poolallocator.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <x86intrin.h>

static constexpr size_t SLOT   = 64;
static constexpr size_t COUNT  = 1 << 16;
static constexpr size_t BATCH  = 64;
static constexpr size_t ROUNDS = 20'000;
static constexpr size_t WARMUP = 2'000;

static constexpr size_t WORKING_SET  = 1024;
static constexpr size_t WS_BATCH     = 64;
static constexpr size_t WS_ROUNDS    = 100'000;
static constexpr size_t WS_WARMUP    = 10'000;

static inline uint64_t tsc_begin() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

static inline uint64_t tsc_end() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence(); //every instruction before was executed before this fence,
    // so we can measure the time of the previous instructions, cutting down on out of order execution effects
    return t;
}

static inline void consume(void* p) {
    asm volatile("" :: "r"(p) : "memory");
}

static double tsc_ghz() {
    uint64_t t0 = tsc_begin();
    timespec ts{0, 200'000'000};
    nanosleep(&ts, nullptr);
    uint64_t t1 = tsc_end();
    return double(t1 - t0) / 200'000'000.0;
}

static void report(const char* name, std::vector<uint64_t>& v, double ghz) {
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (uint64_t x : v) sum += double(x);
    double mean = sum / double(v.size());

    std::printf("%-12s  mean %6.2f cyc (%5.2f ns)   p50 %4llu   p99 %5llu   p99.9 %6llu   max %6llu\n",
                name, mean, mean / ghz,
                (unsigned long long)v[v.size() * 50 / 100],
                (unsigned long long)v[v.size() * 99 / 100],
                (unsigned long long)v[v.size() * 999 / 1000],
                (unsigned long long)v.back());
}

// Benchmark 1: amortized single-slot reuse, batched timing (BATCH=64).
// Measures allocate() alone, cost of one call divided out of a batch of 64 --
// instrumentation overhead becomes negligible, but this reuses the same slot
// every time (LIFO free-list), so it stays hot in L1 and rarely refills.
template <typename Alloc, typename Free>
static void bench_amortized(const char* name, Alloc alloc, Free dealloc, double ghz) {
    std::vector<void*> ptrs(BATCH);
    std::vector<uint64_t> samples;
    samples.reserve(ROUNDS);

    for (size_t r = 0; r < WARMUP; ++r) {
        for (size_t j = 0; j < BATCH; ++j) ptrs[j] = alloc();
        for (size_t j = 0; j < BATCH; ++j) dealloc(ptrs[j]);
    }

    for (size_t r = 0; r < ROUNDS; ++r) {
        uint64_t t0 = tsc_begin();
        for (size_t j = 0; j < BATCH; ++j) ptrs[j] = alloc();
        uint64_t t1 = tsc_end();

        for (size_t j = 0; j < BATCH; ++j) consume(ptrs[j]);
        for (size_t j = 0; j < BATCH; ++j) dealloc(ptrs[j]);

        samples.push_back((t1 - t0) / BATCH);
    }
    report(name, samples, ghz);
}

// Benchmark 2: realistic working-set churn, per-call timing.
// Working set of 1024 slots (larger than glibc's tcache), dealloc'd and
// realloc'd in disjoint 64-wide batches so the same slot is never freed and
// immediately reused. Each alloc/dealloc is timed individually -- full
// rdtscp/lfence overhead lands on every sample, but refill/flush is forced
// every round instead of amortized away.
template <typename Alloc, typename Free>
static void bench_working_set(const char* name, Alloc alloc, Free dealloc, double ghz) {
    std::vector<void*> ptrs(WORKING_SET);
    for (size_t i = 0; i < WORKING_SET; ++i) {
        ptrs[i] = alloc();
    }

    std::vector<uint64_t> samples;
    samples.reserve(WS_ROUNDS);
    size_t head = 0;

    for (size_t r = 0; r < WS_WARMUP; r += WS_BATCH) {
        for (size_t i = 0; i < WS_BATCH; ++i) {
            size_t idx = (head + i) % WORKING_SET;
            dealloc(ptrs[idx]);
        }
        for (size_t i = 0; i < WS_BATCH; ++i) {
            size_t idx = (head + i) % WORKING_SET;
            ptrs[idx] = alloc();
            consume(ptrs[idx]);
        }
        head = (head + WS_BATCH) % WORKING_SET;
    }

    for (size_t r = 0; r < WS_ROUNDS; r += WS_BATCH) {
        uint64_t t0, t1;
        uint64_t dealloc_times[WS_BATCH];
        uint64_t alloc_times[WS_BATCH];

        for (size_t i = 0; i < WS_BATCH; ++i) {
            size_t idx = (head + i) % WORKING_SET;
            t0 = tsc_begin();
            dealloc(ptrs[idx]);
            t1 = tsc_end();
            dealloc_times[i] = t1 - t0;
        }
        for (size_t i = 0; i < WS_BATCH; ++i) {
            size_t idx = (head + i) % WORKING_SET;
            t0 = tsc_begin();
            ptrs[idx] = alloc();
            t1 = tsc_end();
            alloc_times[i] = t1 - t0;
            consume(ptrs[idx]);
        }
        for (size_t i = 0; i < WS_BATCH; ++i) {
            samples.push_back(dealloc_times[i] + alloc_times[i]);
        }
        head = (head + WS_BATCH) % WORKING_SET;
    }

    for (size_t i = 0; i < WORKING_SET; ++i) {
        dealloc(ptrs[i]);
    }
    report(name, samples, ghz);
}

int main() {
    double ghz = tsc_ghz();
    std::printf("TSC %.3f GHz\n\n", ghz);

    PoolAllocator<SLOT, COUNT> pool;

    std::printf("-- amortized (batch=%zu, same-slot reuse) --\n", BATCH);
    bench_amortized("pool", [&] { return pool.allocate(); },
                             [&](void* p) { pool.deallocate(p); }, ghz);
    bench_amortized("malloc", [] { return std::malloc(SLOT); },
                               [](void* p) { std::free(p); }, ghz);

    std::printf("\n-- working set (%zu slots, per-call timing) --\n", WORKING_SET);
    bench_working_set("pool", [&] { return pool.allocate(); },
                                [&](void* p) { pool.deallocate(p); }, ghz);
    bench_working_set("malloc", [] { return std::malloc(SLOT); },
                                  [](void* p) { std::free(p); }, ghz);

    return 0;
}