// Sampling profiler for performance work: when a file named "perf_profile"
// exists next to the exe, the game's main thread and the GPU command thread
// are sampled every millisecond and the busiest functions are written to
// saintsrow_sdk.log every ten seconds.
#pragma once

namespace sr {
void StartProfiler();
void StopProfiler();
}  // namespace sr
