# L1 data cache benchmark

Estimates the **associativity**, **size** and **cache line size** of the CPU's L1 data cache by analyzing latencies of strided memory accesses.

## Build
Requires the Linux system, CMake and C++-20 compiler.

```bash
cmake -S . -B build/ -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build/
```

## Run
It is recommended to pin the process to a CPU core for cleaner results (especially on hybrid architectures)

```sh
taskset -c 0 build/cache_analysis
# associativity = 8
# size          = 32K (8 ways x 4K)
# line size     = 64
taskset -c 0 build/cache_analysis -v    
# print measurement data
```
