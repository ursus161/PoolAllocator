#include "poolallocator.hpp"
#include <cstdio>

int main() {
    PoolAllocator<64, 1024> pool;
    auto p = pool.allocate();
    std::printf("allocated %p\n", p);
    pool.deallocate(p);
    return 0;
}