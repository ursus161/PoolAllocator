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

static inline uint64_t tsc_begin() {
    unsigned aux;
    _mm_lfence();
    return __rdtscp(&aux);
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

    std::printf("%-12s  mean %6.2f cyc (%5.2f ns)   p50 %4llu   p99 %5llu   p99.9 %6llu\n",
                name, mean, mean / ghz,
                (unsigned long long)v[v.size() * 50 / 100],
                (unsigned long long)v[v.size() * 99 / 100],
                (unsigned long long)v[v.size() * 999 / 1000]);
}

template <typename Alloc, typename Free>
static void bench(const char* name, Alloc alloc, Free dealloc, double ghz) {
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

int main() {
    double ghz = tsc_ghz();
    std::printf("TSC %.3f GHz   batch %zu   rounds %zu\n\n", ghz, BATCH, ROUNDS);

    PoolAllocator<SLOT, COUNT> pool;

    bench("pool", [&] { return pool.allocate(); },
                  [&](void* p) { pool.deallocate(p); }, ghz);

    bench("malloc", [] { return std::malloc(SLOT); },
                    [](void* p) { std::free(p); }, ghz);

    return 0;
}