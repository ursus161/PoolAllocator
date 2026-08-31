CXX      := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -Iinclude -pthread -Wno-interference-size

.PHONY: run test tsan bench clean

run:
	$(CXX) $(CXXFLAGS) -O3 -march=native -DNDEBUG \
		src/main.cpp -o pool_run
	./pool_run

test:
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined \
		tests/test_pool.cpp -o test_pool
	./test_pool

tsan:
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread \
		tests/test_pool.cpp -o test_tsan
	./test_tsan

bench:
	$(CXX) $(CXXFLAGS) -O3 -march=native -DNDEBUG \
		bench/bench.cpp -o bench_pool
	./bench_pool

clean:
	rm -f pool_run test_pool test_tsan bench_pool