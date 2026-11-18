// L1 data cache benchmark.
// Based on ideas described in
// "Robust Method to Determine Cache and TLB Characteristics" by V. Chandran
//
// usage: cache_analysis [-v]                       estimate and print the results (-v: also all measurements)

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "config.h"

namespace {

using namespace config;

void print_size(size_t bytes) {
    if (bytes % (1024 * 1024) == 0)  std::printf("%zuM", bytes / (1024 * 1024));
    else if (bytes % 1024 == 0)      std::printf("%zuK", bytes / 1024);
    else                             std::printf("%zu", bytes);
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

size_t mode(std::vector<size_t> values) {
    std::sort(values.begin(), values.end());
    size_t best = values.back(), best_count = 0;
    for (size_t i = 0; i < values.size();) {
        size_t j = i;
        while (j < values.size() && values[j] == values[i]) 
            ++j;
        if (j - i >= best_count) {
            best = values[i];
            best_count = j - i;
        }
        i = j;
    }
    return best;
}

bool nearly_equal(size_t a, size_t b) {
    size_t diff = a > b ? a - b : b - a;
    return diff <= std::max<size_t>(1, static_cast<size_t>(kPlateauTolerance * std::max(a, b)));
}

// Data "consumer" so that computation is not optimized out by the compiler
inline void use_data(const void* p) { asm volatile("" : : "r"(p) : "memory"); }

using elem_t = char*;

struct SetGeometry {
    size_t associativity = 0;      // selected from grid
    size_t raw_associativity = 0;  // as measured
    size_t set_stride = 0;         // smallest stride at which all accesses go to the same set
};

// Search by grid within +-1 range
size_t get_regular_associativity(size_t raw) {
    size_t result = 0;
    int candidates = 0;
    for (size_t g : kAssociativityGrid) {
        size_t diff = g > raw ? g - raw : raw - g;
        if (diff <= 1) {
            result = g;
            ++candidates;
        }
    }
    return candidates == 1 ? result : raw;
}

class CacheAnalyzer {
public:
    explicit CacheAnalyzer(bool verbose) : verbose_(verbose) {
        void* ptr = std::aligned_alloc(sysconf(_SC_PAGESIZE), kBufferSize);
        if (ptr == nullptr)
            throw std::bad_alloc();
        // Keep consistent TLB behavior across runs
        madvise(ptr, kBufferSize, MADV_HUGEPAGE);
        buffer_ = static_cast<char*>(ptr);
        offsets_.reserve(kMaxAccesses + 1);
        latencies_.reserve(kMaxAccesses);
    }
    ~CacheAnalyzer() { std::free(buffer_); }
    CacheAnalyzer(const CacheAnalyzer&) = delete;
    CacheAnalyzer& operator=(const CacheAnalyzer&) = delete;

    void run() {
        SetGeometry geometry;
        for (int attempt = 1; attempt <= kMaxGeometryAttempts && geometry.associativity == 0; ++attempt) {
            if (attempt > 1)
                std::fprintf(stderr, "no stable plateau, retrying (%d/%d)\n", attempt, kMaxGeometryAttempts);
            geometry = measure_set_geometry();
        }
        if (geometry.associativity == 0) {
            std::printf("No stable plateau found: cannot estimate the cache geometry\n");
            return;
        }
        std::printf("associativity = %zu", geometry.associativity);
        if (verbose_ && geometry.associativity != geometry.raw_associativity) {
            std::printf(" (raw %zu)", geometry.raw_associativity);
        }
        std::printf("\n");
        std::printf("size          = ");
        print_size(geometry.associativity * geometry.set_stride);
        std::printf(" (%zu ways x ", geometry.associativity);
        print_size(geometry.set_stride);
        std::printf(")\n");

        size_t line_size = measure_line_size(geometry);
        if (line_size == 0) {
            std::printf("line size     = unknown (no clear step)\n");
            return;
        }
        std::printf("line size     = %zu\n", line_size);
        // std::printf("sets          = %zu\n", geometry.set_stride / line_size);
    }

private:
    // ------------------------------ Chain construction ------------------------------

    // Strided backward access pattern (to reduce prefetching)
    void build_strided_offsets(size_t stride, size_t accesses) {
        offsets_.clear();
        for (size_t i = accesses; i-- > 0;)
            offsets_.push_back(i * stride);
    }

    // `associativity` lines of one set (to fill it), plus extra access at `probe_offset`
    // to a neighbour line
    void build_line_probe_offsets(size_t stride, size_t associativity, size_t probe_offset) {
        build_strided_offsets(stride, associativity);
        offsets_.insert(offsets_.begin(), associativity * stride + probe_offset);
    }

    // Make buffer[offsets_[i]] point to buffer[offsets_[i + 1]], cyclically
    // Inrtroducing memory dependencies prevents the compiler from optimizing the loads
    elem_t* link_chain() {
        char* base = buffer_ + kChainBase;
        for (size_t i = 0; i < offsets_.size(); ++i)
            *reinterpret_cast<elem_t*>(base + offsets_[i]) = base + offsets_[(i + 1) % offsets_.size()];
        return reinterpret_cast<elem_t*>(base + offsets_[0]);
    }

    // ------------------------------ Timing ------------------------------

    static elem_t* walk(elem_t* cur, size_t iterations) {
        for (size_t i = 0; i < iterations; ++i) {
            static_assert(kLoadsPerIteration == 10, "loop must be unrolled at kLoadsPerIteration");
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
            cur = reinterpret_cast<elem_t*>(*cur);
        }
        return cur;
    }

    double measure_latency(size_t iterations, int repeats) {
        elem_t* cur = link_chain();
        cur = walk(cur, kWarmupIterations + offsets_.size());
        double best = 1e30;
        for (int r = 0; r < repeats; ++r) {
            auto start = std::chrono::steady_clock::now();
            cur = walk(cur, iterations);
            auto end = std::chrono::steady_clock::now();
            double ns = std::chrono::duration<double, std::nano>(end - start).count();
            best = std::min(best, ns / (iterations * kLoadsPerIteration));
        }
        use_data(cur);
        return best;
    }

    // ------------------------------ Geometry analysis ------------------------------

    // The number of accesses that overflow a cache set.
    // Detected by rapid latency increase ("jump")
    size_t detect_jump() const {
        std::vector<double> head(latencies_.begin(), latencies_.begin() + kBaselinePoints);
        double threshold = median(std::move(head)) * kSlowdown;
        for (size_t i = 0; i + kSustainedPoints <= latencies_.size(); ++i) {
            bool sustained = true;
            for (size_t j = i; j < i + kSustainedPoints && sustained; ++j)
                sustained = latencies_[j] > threshold;
            if (sustained)
                return i;
        }
        return 0;
    }

    // The jump point halves with every stride doubling until it stabilizes.
    // At this point all accesses go to the same cache set and we get the set capacity (i.e. associativity)
    static SetGeometry find_plateau(const std::vector<size_t>& strides, const std::vector<size_t>& jumps) {
        for (size_t i = 0; i + 2 < jumps.size(); ++i) {
            if (jumps[i] < 2 || !nearly_equal(jumps[i], jumps[i + 1]) || !nearly_equal(jumps[i], jumps[i + 2]))
                continue;
            std::vector<size_t> plateau;
            for (size_t j = i; j < jumps.size() && nearly_equal(jumps[i], jumps[j]); ++j)
                plateau.push_back(jumps[j]);
            size_t value = mode(plateau);
            // Check that we decreased roughly by half before stabilizing
            // (previous jump is approx. 2 * plateau)
            if (i > 0 && jumps[i - 1] != 0) {
                double ratio = static_cast<double>(jumps[i - 1]) / value;
                if (ratio < kHalvingMin || ratio > kHalvingMax)
                    continue;
            }

            SetGeometry geometry;
            geometry.raw_associativity = value;
            geometry.associativity = get_regular_associativity(value);
            geometry.set_stride = strides[i];
            return geometry;
        }
        return {};
    }

    // For each stride (increasing by a power of 2), determine a jump point.
    // Then, determine the stride at which the jump points stabilize.
    // The point gives cache associativity, the stride is used to estimate cache size
    SetGeometry measure_set_geometry() {
        std::vector<size_t> strides, jumps;
        constexpr size_t n_strides = std::bit_width(kMaxStride / kMinStride) - 1;
        strides.reserve(n_strides);
        jumps.reserve(n_strides);
        for (size_t stride = kMinStride; stride <= kMaxStride; stride *= 2) {
            latencies_.clear();
            for (size_t accesses = 1; accesses <= kMaxAccesses; ++accesses) {
                build_strided_offsets(stride, accesses);
                latencies_.push_back(measure_latency(kIterations, kRepeats));
            }
            strides.push_back(stride);
            jumps.push_back(detect_jump());
            if (verbose_) {
                std::printf("stride ");
                print_size(stride);
                std::printf(": ns/load for 1..%zu accesses:", kMaxAccesses);
                for (double ns : latencies_)
                    std::printf(" %.2f", ns);
                std::printf("\n");
            }
        }
        if (verbose_) {
            std::printf("latency \"jump\" point per stride:");
            for (size_t i = 0; i < strides.size(); ++i) {
                std::printf(" ");
                print_size(strides[i]);
                std::printf("=%zu", jumps[i]);
            }
            std::printf("\n\n");
        }
        return find_plateau(strides, jumps);
    }

    // ------------------------------ Cache line analysis ------------------------------

    // If the probe access hits the same cache set as the others - latency is high due to line eviction.
    // If we land in the next line, latency is low as we hit the fresh cache set. 
    // We determine the minimal step at which this happens.
    size_t measure_line_size(const SetGeometry& geometry) {
        std::vector<size_t> probe_offsets;
        probe_offsets.reserve(kMaxLineProbeOffset);
        latencies_.clear();
        for (size_t d = 0; d <= kMaxLineProbeOffset && d < geometry.set_stride; d += kLineProbeStep) {
            build_line_probe_offsets(geometry.set_stride, geometry.associativity, d);
            probe_offsets.push_back(d);
            latencies_.push_back(measure_latency(kIterations, kLineProbeRepeats));
        }
        if (verbose_) {
            std::printf("line size probe (stride ");
            print_size(geometry.set_stride);
            std::printf(", %zu accesses): probe offset -> ns/load\n", geometry.associativity);
            for (size_t i = 0; i < latencies_.size(); ++i)
                std::printf("%zu:%.2f ", probe_offsets[i], latencies_[i]);
            std::printf("\n\n");
        }

        auto first = latencies_.begin() + kLineProbeRefBegin;
        double slow = median({first, first + kLineProbeRefPoints});  // latency of evicting a line
        double fast = median({latencies_.end() - kLineProbeRefPoints, latencies_.end()});  // latency of hitting a fresh set
        if (slow < fast * kSlowdown)
            return 0;
        // Detect the "jump" point: the first offset from which the latency stays low
        // for several accesses
        double middle = (slow + fast) / 2;
        for (size_t i = kLineProbeRefBegin + kLineProbeRefPoints; i + kLineProbeSustainedPoints <= latencies_.size(); ++i) {
            bool sustained = true;
            for (size_t j = i; j < i + kLineProbeSustainedPoints && sustained; ++j)
                sustained = latencies_[j] < middle;
            if (sustained)
                return probe_offsets[i];
        }
        return 0;
    }

    bool verbose_;
    char* buffer_ = nullptr;
    // Preallocate reused memory
    std::vector<size_t> offsets_;
    std::vector<double> latencies_;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        CacheAnalyzer(argc > 1 && std::strcmp(argv[1], "-v") == 0).run();
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "cannot allocate the test buffer\n");
        return 1;
    }
}
