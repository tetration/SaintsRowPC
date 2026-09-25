// Button glyphs that follow the input device.
//
// The game draws its button prompts (HUD help, pause menu and creator tabs,
// "Press <btn> ..." text) from DXT textures: HUD sprite sheets in
// interface-backend.peg, px_btn* textures in px_base / px_pause_menu_base and
// button characters in the px_thin fonts. kbm_ui.bin next to the exe holds,
// for each of those textures, the DXT blocks covering the button pictures:
// the original and a keyboard/mouse version for each control context (menus,
// on foot, in a vehicle, character creator). Setup makes the file on the
// player's PC from their own game files (tools/glyphgen).
//
// A background thread finds the loaded textures in guest physical memory
// (pegs stay loaded; the pause menu peg comes and goes) and writes the set
// that matches the device the player used last. The GPU texture cache sees
// the write through its memory watch and uploads the new picture.

#include "glyphs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sr {
namespace {

constexpr uint32_t kPhysicalSize = 0x20000000u;
constexpr unsigned kSamples = 16;

std::atomic<uint8_t*> g_base{nullptr};
std::atomic<int> g_device{0};   // 0 controller, 1 keyboard/mouse
std::atomic<int> g_context{1};
std::once_flag g_start;

uint64_t Key(const uint8_t* page) {
  // Sixteen 8-byte samples spread over the first 4 KB of the texture.
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (unsigned i = 0; i < kSamples; ++i) {
    uint64_t v;
    std::memcpy(&v, page + 0x40 + i * 0xF8, 8);
    h = (h ^ v) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h;
}

// Textures from kbm_ui.bin. Only the DXT blocks covering the button
// pictures are stored.
struct FontTex {
  char name[49]{};
  uint32_t size = 0;
  std::vector<uint32_t> offsets;           // block offsets in the texture
  std::vector<std::vector<uint8_t>> data;  // per version: 16 bytes per block
  std::vector<std::vector<uint8_t>> pages; // per version: first 4 KB
  bool small = false;                      // found by its first 64 bytes only
  uint32_t key_offset = 0;                 // where the 4 KB it is found by starts
};
std::vector<FontTex> g_fonts;
std::unordered_multimap<uint64_t, unsigned> g_font_keys;  // first-page key -> font
std::unordered_multimap<uint64_t, unsigned> g_small_keys; // first-64-bytes key -> texture

uint64_t KeySmall(const uint8_t* p) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (int i = 0; i < 64; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * 0x100000001B3ull;
    h ^= h >> 31;
  }
  return h;
}

bool LoadFonts() {
  const auto path = rex::filesystem::GetExecutableFolder() / "kbm_ui.bin";
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    REXLOG_INFO("Glyphs: {} not found; button prompts stay as controller buttons", path.string());
    return false;
  }
  char magic[4];
  uint32_t count = 0, versions = 0;
  bool ok = std::fread(magic, 1, 4, f) == 4 && !std::memcmp(magic, "SRG3", 4) &&
            std::fread(&count, 4, 1, f) == 1 && std::fread(&versions, 4, 1, f) == 1 && count < 64 &&
            versions >= 1 && versions <= 8;
  for (uint32_t i = 0; ok && i < count; ++i) {
    FontTex t;
    uint32_t blocks = 0;
    std::vector<uint8_t> page(4096);
    ok = std::fread(t.name, 1, 48, f) == 48 && std::fread(&t.size, 4, 1, f) == 1 &&
         std::fread(&blocks, 4, 1, f) == 1 && blocks < 100000;
    uint32_t flags = 0;
    ok = ok && std::fread(&flags, 4, 1, f) == 1;
    if (ok && (flags & 2)) ok = std::fread(&t.key_offset, 4, 1, f) == 1 && t.key_offset % 4096 == 0;
    ok = ok && std::fread(page.data(), 1, 4096, f) == 4096;
    t.small = (flags & 1) != 0;
    ok = ok && (t.small || t.size >= t.key_offset + 4096u);
    if (!ok) break;
    t.offsets.resize(blocks);
    ok = std::fread(t.offsets.data(), 4, blocks, f) == blocks;
    for (uint32_t v = 0; ok && v < versions; ++v) {
      std::vector<uint8_t> d(size_t(blocks) * 16);
      ok = std::fread(d.data(), 1, d.size(), f) == d.size();
      t.data.push_back(std::move(d));
    }
    for (uint32_t o : t.offsets) ok = ok && o + 16 <= t.size;
    if (!ok) break;
    // Key page as it looks in each version (for finding it again after a swap).
    for (uint32_t v = 0; v < versions; ++v) {
      std::vector<uint8_t> pv = page;
      for (size_t b = 0; b < t.offsets.size(); ++b)
        if (t.offsets[b] >= t.key_offset && t.offsets[b] + 16 <= t.key_offset + 4096u)
          std::memcpy(pv.data() + (t.offsets[b] - t.key_offset), t.data[v].data() + b * 16, 16);
      t.pages.push_back(std::move(pv));
    }
    g_fonts.push_back(std::move(t));
  }
  std::fclose(f);
  if (!ok) { g_fonts.clear(); REXLOG_WARN("Glyphs: {} is damaged; not used", path.string()); return false; }
  for (unsigned i = 0; i < g_fonts.size(); ++i)
    for (auto& pv : g_fonts[i].pages) {
      auto& keys = g_fonts[i].small ? g_small_keys : g_font_keys;
      const uint64_t k = g_fonts[i].small ? KeySmall(pv.data()) : Key(pv.data());
      bool dup = false;
      auto range = keys.equal_range(k);
      for (auto it = range.first; it != range.second; ++it) dup = dup || it->second == i;
      if (!dup) keys.emplace(k, i);
    }
  REXLOG_INFO("Glyphs: {} UI textures with keyboard button pictures loaded", g_fonts.size());
  return !g_fonts.empty();
}

// Which version all of a font's icon blocks currently show, or -1.
int FontVersion(const uint8_t* tex, const FontTex& t, int prefer = -1) {
  // Versions can be identical (a picture that looks the same in two
  // contexts): report the wanted one when it matches, so it isn't rewritten.
  if (prefer >= 0 && size_t(prefer) < t.data.size()) {
    bool all = true;
    for (size_t b = 0; b < t.offsets.size() && all; ++b)
      all = !std::memcmp(tex + t.offsets[b], t.data[prefer].data() + b * 16, 16);
    if (all) return prefer;
  }
  for (size_t v = 0; v < t.data.size(); ++v) {
    bool all = true;
    for (size_t b = 0; b < t.offsets.size() && all; ++b)
      all = !std::memcmp(tex + t.offsets[b], t.data[v].data() + b * 16, 16);
    if (all) return int(v);
  }
  return -1;
}

struct FoundFont { uint32_t address; unsigned font; };
uint32_t PhysOf(uint32_t a) { return a >= 0xE0000000u ? (a - 0xE0000000u + 0x1000u) : (a & 0x1FFFFFFFu); }
std::vector<FoundFont> g_found_fonts;

#ifdef _WIN32
// Host pointer for a guest physical-view address (the 0xE0000000 view sits
// 4 KB further on in the host mapping, as REX_RAW_ADDR does).
uint8_t* HostPointer(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

// Physical memory is visible through three guest views (0xA0000000 4 KB
// pages, 0xC0000000 16 MB pages, 0xE0000000 4 KB pages). An allocation is
// only accessible through the view it was made in, so all three are scanned.
size_t g_scanned_bytes[3] = {};
void ScanView(uint8_t* base, uint32_t view, size_t& scanned) {
  uint8_t* start = HostPointer(base, view);
  uint8_t* end = start + kPhysicalSize;
  for (uint8_t* region = start; region < end;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(region, &mbi, sizeof(mbi))) break;
    uint8_t* rend = std::min(end, static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
    if (rend <= region) break;
    const DWORD p = mbi.Protect & 0xFF;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                          (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_EXECUTE_READ ||
                           p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY);
    if (readable) {
      scanned += size_t(rend - region);
      for (uint8_t* page = region; page + 0x1000 <= rend; page += 0x1000) {
        const uint64_t key = Key(page);
        auto franges = g_font_keys.equal_range(key);
        for (auto it = franges.first; it != franges.second; ++it) {
          const FontTex& t = g_fonts[it->second];
          if (size_t(page - region) < t.key_offset) continue;
          uint8_t* tex = page - t.key_offset;
          if (tex + t.size > rend || FontVersion(tex, t) < 0) continue;
          const uint32_t address = view + uint32_t(tex - start);
          bool known = false;
          for (auto& f : g_found_fonts) known = known || PhysOf(f.address) == PhysOf(address);
          if (!known) {
            g_found_fonts.push_back({address, it->second});
            REXLOG_INFO("Glyphs: font {} found at {:08X}", t.name, address);
          }
        }
        auto sranges = g_small_keys.equal_range(KeySmall(page));
        for (auto it = sranges.first; it != sranges.second; ++it) {
          const FontTex& t = g_fonts[it->second];
          if (page + t.size > rend || FontVersion(page, t) < 0) continue;
          const uint32_t address = view + uint32_t(page - start);
          bool known = false;
          for (auto& f : g_found_fonts) known = known || PhysOf(f.address) == PhysOf(address);
          if (!known) {
            g_found_fonts.push_back({address, it->second});
            REXLOG_INFO("Glyphs: {} found at {:08X}", t.name, address);
          }
        }
      }
    }
    region = rend;
  }
}

void Scan(uint8_t* base) {
  const uint32_t views[3] = {0xA0000000u, 0xC0000000u, 0xE0000000u};
  for (int i = 0; i < 3; ++i) { g_scanned_bytes[i] = 0; ScanView(base, views[i], g_scanned_bytes[i]); }
}

// Scanning reads a little of every committed physical page (about 1.5 GB of
// address space), so it only runs when keyboard pictures are wanted: a quick
// series of scans after the device or context changes (a menu's textures load
// a moment after it opens), then one every few seconds for textures that load
// later. With a controller nothing needs finding: textures show their
// original pictures when loaded, and the ones already switched are restored
// from the list below.
void Worker() {
  using namespace std::chrono;
  static constexpr int kQuickScansMs[] = {0, 300, 700, 1500, 3000};
  constexpr int kQuickScans = int(sizeof(kQuickScansMs) / sizeof(kQuickScansMs[0]));
  auto next_scan = steady_clock::now();
  int quick = 0;
  int last_want = -1;
  int last_logged_count = -1;
  for (;;) {
    std::this_thread::sleep_for(milliseconds(100));
    uint8_t* base = g_base.load();
    if (!base) continue;
    const auto now = steady_clock::now();
    const int want_now = g_device.load() ? 1 + g_context.load() : 0;
    if (want_now != last_want) {
      last_want = want_now;
      quick = 0;
      next_scan = now;
    }
    if (want_now != 0 && now >= next_scan) {
      if (quick + 1 < kQuickScans) {
        next_scan = now + milliseconds(kQuickScansMs[quick + 1] - kQuickScansMs[quick]);
        ++quick;
      } else {
        next_scan = now + seconds(5);
      }
      Scan(base);
      if (int(g_found_fonts.size()) != last_logged_count) {
        last_logged_count = int(g_found_fonts.size());
        REXLOG_INFO("Glyphs: {} UI textures found (scanned {} / {} / {} MB)", g_found_fonts.size(),
                    g_scanned_bytes[0] >> 20, g_scanned_bytes[1] >> 20, g_scanned_bytes[2] >> 20);
      }
    }
    const int want = g_device.load() ? 1 + g_context.load() : 0;
    for (size_t i = 0; i < g_found_fonts.size();) {
      const FontTex& t = g_fonts[g_found_fonts[i].font];
      uint8_t* p = HostPointer(base, g_found_fonts[i].address);
      const int have = FontVersion(p, t, want);
      if (have < 0) {
        g_found_fonts.erase(g_found_fonts.begin() + i);
        continue;
      }
      if (have != want && want < int(t.data.size())) {
        for (size_t b = 0; b < t.offsets.size(); ++b)
          std::memcpy(p + t.offsets[b], t.data[want].data() + b * 16, 16);
        // Tell the GPU side the texture memory changed so it is re-read.
        // The watch may be registered through any of the physical views, so
        // report the change through all three (the 0xE0000000 view is
        // offset by 4 KB).
        if (auto* memory = rex::system::kernel_memory()) {
          const uint32_t a = g_found_fonts[i].address;
          const uint32_t phys = a >= 0xE0000000u ? (a - 0xE0000000u + 0x1000u) : (a & 0x1FFFFFFFu);
          const uint32_t views[3] = {0xA0000000u + phys, 0xC0000000u + phys, 0xE0000000u + phys - 0x1000u};
          for (uint32_t v : views)
            memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(), v,
                                                   t.size, true, false);
        }
        REXLOG_INFO("Glyphs: {} at {:08X} switched to {} (was {})", t.name, g_found_fonts[i].address,
                    want == 0 ? "controller" : want == 1 ? "keyboard (menus)" : want == 2 ? "keyboard (on foot)" : want == 3 ? "keyboard (vehicle)" : "keyboard (creator)",
                    have);
      }
      ++i;
    }
  }
}
#endif

}  // namespace

void GlyphsNoteInput(uint8_t* base, bool controller_used, bool kbm_used, GlyphContext context) {
#ifdef _WIN32
  std::call_once(g_start, [] {
    if (LoadFonts()) std::thread(Worker).detach();
  });
  if (g_fonts.empty()) return;
  g_base.store(base, std::memory_order_relaxed);
  if (controller_used && !kbm_used) g_device.store(0, std::memory_order_relaxed);
  else if (kbm_used && !controller_used) g_device.store(1, std::memory_order_relaxed);
  g_context.store(int(context), std::memory_order_relaxed);
#else
  (void)base; (void)controller_used; (void)kbm_used; (void)context;
#endif
}

}  // namespace sr
