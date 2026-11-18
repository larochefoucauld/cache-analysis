#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>

namespace config {

// Memory
constexpr size_t kBufferSize = 128 * 1024 * 1024;
// Start accessed memory region away from the allocator metadata placed at the start of the buffer
constexpr size_t kChainBase = kBufferSize / 2;

// Access pattern
constexpr size_t kMinStride = sizeof(void*);
constexpr size_t kMaxStride = 256 * 1024;
constexpr size_t kMaxAccesses = 128;

// Measurements
constexpr size_t kLoadsPerIteration = 10;
constexpr size_t kIterations = 100000;
constexpr size_t kWarmupIterations = 1000;
constexpr int kRepeats = 7;
constexpr int kLineProbeRepeats = 3 * kRepeats;

// Detection
constexpr double kSlowdown = 1.15;          // slowdown threshold w.r.t the base latency
constexpr size_t kSustainedPoints = 3;      // minimal number of points the plateau must hold
constexpr size_t kBaselinePoints = 3;       // used to estimate the base latency
constexpr double kPlateauTolerance = 0.1;   // used to determine plateau of latency "jumps" as the stride increases
constexpr double kHalvingMin = 1.7;
constexpr double kHalvingMax = 2.3;

// Typical associativities
constexpr size_t kAssociativityGrid[] = {2, 4, 8, 12, 16, 20, 24, 32};

// Retry geometry analysis this number of times if it does not converge
constexpr int kMaxGeometryAttempts = 3;

// Line size analysis
constexpr size_t kMaxLineProbeOffset = 256;
constexpr size_t kLineProbeStep = sizeof(void*);
// On some machines stepping the same pure stride as a probe is suspiciously fast (prefetching?).
// So we use the next few steps to get reliable latency
constexpr size_t kLineProbeRefBegin = 1;
constexpr size_t kLineProbeRefPoints = 4;
// The step must hold for this many consecutive probe offsets
constexpr size_t kLineProbeSustainedPoints = 4;

}  // namespace config

#endif // CONFIG_H
