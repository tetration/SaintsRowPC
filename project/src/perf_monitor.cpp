#include "perf_monitor.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/flags.h>

#include <filesystem>
#include <iterator>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace sr {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_running{false};
std::thread g_thread;

// Frame statistics, written by the present thread, read and reset by the
// monitor thread.
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_frame_us_sum{0};
std::atomic<uint64_t> g_frame_us_max{0};
std::atomic<uint64_t> g_present_us_sum{0};
std::atomic<uint64_t> g_bucket[5];  // <=17.5, <=21, <=26, <=34.5, >34.5 ms
std::atomic<std::thread::id> g_main_thread{};
std::atomic<uint64_t> g_counters[kPerfCounterCount];
std::mutex g_wait_mutex;
std::map<uint32_t, std::pair<uint64_t, uint64_t>> g_waits;  // caller -> (count, us)
Clock::time_point g_last_frame;
Clock::time_point g_present_start;
bool g_have_last = false;

uint64_t Us(Clock::duration d) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

#ifdef _WIN32
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::string ThreadName(HANDLE thread) {
  static auto fn = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!fn) return {};
  PWSTR wide = nullptr;
  if (FAILED(fn(thread, &wide)) || !wide) return {};
  char narrow[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
  LocalFree(wide);
  return narrow;
}

struct ThreadSample {
  uint64_t cpu_100ns;
  std::string name;
};

std::unordered_map<DWORD, ThreadSample> SampleThreads() {
  std::unordered_map<DWORD, ThreadSample> out;
  DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c, e, k, u;
    if (GetThreadTimes(h, &c, &e, &k, &u)) {
      uint64_t kt = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime;
      uint64_t ut = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
      out[te.th32ThreadID] = {kt + ut, ThreadName(h)};
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  return out;
}
#endif

void MonitorLoop() {
#ifdef _WIN32
  auto prev_threads = SampleThreads();
#endif
  auto prev_time = Clock::now();
  while (g_running.load()) {
    for (int i = 0; i < 20 && g_running.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto now = Clock::now();
    double wall_s = std::chrono::duration<double>(now - prev_time).count();
    prev_time = now;

    uint64_t frames = g_frames.exchange(0);
    uint64_t sum = g_frame_us_sum.exchange(0);
    uint64_t mx = g_frame_us_max.exchange(0);
    uint64_t present = g_present_us_sum.exchange(0);
    uint64_t b[5];
    for (int i = 0; i < 5; ++i) b[i] = g_bucket[i].exchange(0);

    char line[256];
    std::snprintf(line, sizeof(line),
                  "PERF fps %.1f | frame avg %.1f max %.1f ms | in present %.1f ms | "
                  "<=17.5:%llu <=21:%llu <=26:%llu <=34.5:%llu >34.5:%llu",
                  frames / wall_s, frames ? sum / 1000.0 / frames : 0.0, mx / 1000.0,
                  frames ? present / 1000.0 / frames : 0.0, (unsigned long long)b[0],
                  (unsigned long long)b[1], (unsigned long long)b[2], (unsigned long long)b[3],
                  (unsigned long long)b[4]);
    std::string text = line;
    {
      char part[160];
      std::snprintf(part, sizeof(part), " | cmdbufs %.0f/s tiling exec %.0f/s replay %.0f/s",
                    g_counters[kPerfCommandBuffers].exchange(0) / wall_s,
                    g_counters[kPerfTilingExecutor].exchange(0) / wall_s,
                    g_counters[kPerfTilingReplay].exchange(0) / wall_s);
      text += part;
    }
    {
      std::lock_guard<std::mutex> lock(g_wait_mutex);
      std::vector<std::pair<uint64_t, uint32_t>> waits;
      for (auto& [caller, v] : g_waits) waits.emplace_back(v.second, caller);
      std::sort(waits.rbegin(), waits.rend());
      text += " | main waits:";
      for (size_t i = 0; i < waits.size() && i < 5; ++i) {
        char part[96];
        std::snprintf(part, sizeof(part), " %08X %.1fms/f x%llu", waits[i].second,
                      frames ? waits[i].first / 1000.0 / frames : 0.0,
                      (unsigned long long)g_waits[waits[i].second].first);
        text += part;
      }
      g_waits.clear();
    }

#ifdef _WIN32
    auto threads = SampleThreads();
    struct Busy {
      double pct;
      DWORD tid;
      const std::string* name;
    };
    std::vector<Busy> busy;
    double total = 0;
    for (const auto& [tid, sample] : threads) {
      auto it = prev_threads.find(tid);
      uint64_t before = it != prev_threads.end() ? it->second.cpu_100ns : 0;
      double pct = double(sample.cpu_100ns - before) / (wall_s * 1e7) * 100.0;
      total += pct;
      if (pct >= 3.0) busy.push_back({pct, tid, &sample.name});
    }
    std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.pct > b.pct; });
    char part[160];
    std::snprintf(part, sizeof(part), " | cpu total %.0f%%:", total);
    text += part;
    for (size_t i = 0; i < busy.size() && i < 10; ++i) {
      std::snprintf(part, sizeof(part), " [%s#%lu %.0f%%]",
                    busy[i].name->empty() ? "?" : busy[i].name->c_str(),
                    (unsigned long)busy[i].tid, busy[i].pct);
      text += part;
    }
    prev_threads = std::move(threads);
#endif
    REXLOG_INFO("{}", text);
  }
}

}  // namespace

void StartPerfMonitor() {
  FILE* f = std::fopen("perf_log", "rb");
  if (!f) return;
  std::fclose(f);
  g_enabled = true;
  g_running = true;
  g_thread = std::thread(MonitorLoop);
  REXLOG_INFO("Performance log enabled");
}

void StopPerfMonitor() {
  if (!g_running.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}

void PerfCount(PerfCounter counter) {
  if (g_enabled.load(std::memory_order_relaxed)) {
    g_counters[counter].fetch_add(1, std::memory_order_relaxed);
  }
}

bool PerfTrackWaits() {
  return g_enabled.load(std::memory_order_relaxed) &&
         g_main_thread.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

void PerfRecordWait(uint32_t guest_caller, uint64_t microseconds) {
  std::lock_guard<std::mutex> lock(g_wait_mutex);
  auto& v = g_waits[guest_caller];
  v.first++;
  v.second += microseconds;
}

void PerfFrameBegin() {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  g_main_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  auto now = Clock::now();
  if (g_have_last) {
    uint64_t us = Us(now - g_last_frame);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_frame_us_sum.fetch_add(us, std::memory_order_relaxed);
    uint64_t mx = g_frame_us_max.load(std::memory_order_relaxed);
    while (us > mx && !g_frame_us_max.compare_exchange_weak(mx, us)) {
    }
    int bucket = us <= 17500 ? 0 : us <= 21000 ? 1 : us <= 26000 ? 2 : us <= 34500 ? 3 : 4;
    g_bucket[bucket].fetch_add(1, std::memory_order_relaxed);
  }
  g_last_frame = now;
  g_have_last = true;
  g_present_start = now;
}

void PerfFrameEnd() {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  g_present_us_sum.fetch_add(Us(Clock::now() - g_present_start), std::memory_order_relaxed);
}

}  // namespace sr

namespace sr {

void LimitFrameRate(double fps) {
  if (fps <= 0) return;
  using namespace std::chrono;
  static steady_clock::time_point next{};
  const auto interval = duration_cast<steady_clock::duration>(duration<double>(1.0 / fps));
  auto now = steady_clock::now();
  if (next.time_since_epoch().count() == 0 || now - next > interval * 4) {
    // First frame, or far behind (loading screen, hitch): start over.
    next = now + interval;
    return;
  }
  if (now < next) {
#ifdef _WIN32
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                                 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    auto remaining = next - now;
    // Sleep most of the way, then spin for the last half millisecond.
    auto sleep_for = remaining - microseconds(500);
    if (timer && sleep_for > microseconds(0)) {
      LARGE_INTEGER due;
      due.QuadPart = -int64_t(duration_cast<nanoseconds>(sleep_for).count() / 100);
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
      }
    }
#else
    std::this_thread::sleep_until(next - microseconds(500));
#endif
    while (steady_clock::now() < next) {
      std::this_thread::yield();
    }
  }
  next += interval;
  // Don't bank time from slow frames: never let the schedule fall behind now.
  now = steady_clock::now();
  if (next < now) next = now;
}

}  // namespace sr

namespace sr {

std::atomic<int> g_fps_cap{60};

namespace {
constexpr int kFpsCaps[] = {30, 60, 90, 120};

std::filesystem::path FpsCapFile() {
  return rex::filesystem::GetExecutableFolder() / "fps_cap.txt";
}

// The game presents on the guest's vertical blank, which comes at the video
// mode's 60 Hz. Above 60, vertical blanks come at the cap's rate instead (as
// on a faster display) and the limiter paces the frames.
void ApplyFpsCap(int cap) {
  g_fps_cap.store(cap, std::memory_order_relaxed);
  REXCVAR_SET(vsync, true);
  REXCVAR_SET(guest_vblank_rate, cap > 60 ? double(cap) : 0.0);
}
}  // namespace

void LoadFpsCap() {
  int cap = 60;
  if (FILE* f = std::fopen(FpsCapFile().string().c_str(), "r")) {
    int v = 0;
    if (std::fscanf(f, "%d", &v) == 1) {
      for (int c : kFpsCaps) if (c == v) cap = v;
    }
    std::fclose(f);
  }
  ApplyFpsCap(cap);
}

int CycleFpsCap() {
  int cur = g_fps_cap.load(std::memory_order_relaxed);
  int next = kFpsCaps[0];
  for (size_t i = 0; i < std::size(kFpsCaps); ++i) {
    if (kFpsCaps[i] == cur) { next = kFpsCaps[(i + 1) % std::size(kFpsCaps)]; break; }
  }
  ApplyFpsCap(next);
  if (FILE* f = std::fopen(FpsCapFile().string().c_str(), "w")) {
    std::fprintf(f, "%d\n", next);
    std::fclose(f);
  }
  REXLOG_INFO("FPS cap: {}", next);
  return next;
}

}  // namespace sr
