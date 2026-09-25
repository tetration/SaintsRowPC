// glyphgen - builds dist/kbm_ui.bin, the keyboard/mouse button pictures the
// game shows instead of controller buttons (project/src/glyphs.cpp).
//
//   glyphgen <game packfiles folder> <art.txt> <output kbm_ui.bin>
//
// Setup runs this on the player's PC. It reads the button textures and font
// textures from the player's own game files, pastes our pictures (art.png,
// drawn from scratch by make_art.py) over the controller buttons, re-encodes
// the affected DXT blocks and writes the result. No game data is stored in
// this repository; it only exists in the player's dist folder.
//
// Output format (little-endian), read by glyphs.cpp LoadFonts():
//   "SRG3", u32 texture count, u32 versions (1 original + one per context)
//   per texture: char[48] label, u32 size, u32 block count, u32 flags (bit 0 =
//   small, bit 1 = key offset follows), [u32 key offset], 4096 bytes of the
//   original texture from the key offset (default 0; to find it in memory),
//   u32 block offsets[count], then per version 16 bytes per block.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "packfile.h"

namespace fs = std::filesystem;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::fprintf(stderr, "glyphgen: %s\n", message.c_str());
  std::exit(1);
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

// ---- art.png ---------------------------------------------------------------------
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // RGBA
  uint8_t* At(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
  const uint8_t* At(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

// 8-bit RGBA, non-interlaced PNG (what make_art.py writes).
Image LoadPng(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (d.size() < 8 || std::memcmp(d.data(), kSig, 8)) Fail("not a PNG file: " + path.string());
  Image img;
  std::vector<uint8_t> idat;
  for (size_t p = 8; p + 12 <= d.size();) {
    const uint32_t len = Be32(&d[p]);
    if (p + 12 + len > d.size()) Fail("damaged PNG: " + path.string());
    const char* type = reinterpret_cast<const char*>(&d[p + 4]);
    const uint8_t* body = &d[p + 8];
    if (!std::memcmp(type, "IHDR", 4)) {
      img.w = int(Be32(body));
      img.h = int(Be32(body + 4));
      if (body[8] != 8 || body[9] != 6 || body[12] != 0) Fail("art.png must be 8-bit RGBA, not interlaced");
    } else if (!std::memcmp(type, "IDAT", 4)) {
      idat.insert(idat.end(), body, body + len);
    } else if (!std::memcmp(type, "IEND", 4)) {
      break;
    }
    p += 12 + len;
  }
  if (img.w <= 0 || img.h <= 0 || img.w > 8192 || img.h > 8192) Fail("bad PNG size: " + path.string());
  const size_t stride = size_t(img.w) * 4;
  std::vector<uint8_t> raw((stride + 1) * img.h);
  uLongf raw_len = uLongf(raw.size());
  if (uncompress(raw.data(), &raw_len, idat.data(), uLong(idat.size())) != Z_OK || raw_len != raw.size())
    Fail("damaged PNG data: " + path.string());
  img.px.resize(stride * img.h);
  std::vector<uint8_t> prev(stride, 0);
  for (int y = 0; y < img.h; ++y) {
    const uint8_t filter = raw[y * (stride + 1)];
    const uint8_t* src = &raw[y * (stride + 1) + 1];
    uint8_t* row = &img.px[y * stride];
    for (size_t i = 0; i < stride; ++i) {
      const int a = i >= 4 ? row[i - 4] : 0, b = prev[i], c = i >= 4 ? prev[i - 4] : 0;
      int v = src[i];
      switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: {
          const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: Fail("damaged PNG filter: " + path.string());
      }
      row[i] = uint8_t(v);
    }
    std::memcpy(prev.data(), row, stride);
  }
  return img;
}

// ---- art.txt ---------------------------------------------------------------------
struct Paste {
  int ctx, x, y, w, h, ax, ay;
};
struct TextureSpec {
  std::string label, pack, file, tex;
  uint32_t nbytes = 0, fmt = 0;
  int w = 0, h = 0;
  bool small = false, dark = false;
  uint32_t key = 0;  // offset of the 4 KB the game finds the texture by
  std::vector<Paste> pastes;
};

std::vector<TextureSpec> LoadList(const fs::path& path, int& contexts) {
  std::ifstream in(path);
  if (!in) Fail("cannot open " + path.string());
  std::vector<TextureSpec> list;
  contexts = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    std::string kind;
    s >> kind;
    if (kind == "contexts") {
      s >> contexts;
    } else if (kind == "texture") {
      TextureSpec t;
      std::string fmt;
      int small = 0, dark = 0;
      s >> t.label >> t.pack >> t.file >> t.tex >> t.nbytes >> fmt >> t.w >> t.h >> small >> dark;
      t.fmt = uint32_t(std::stoul(fmt, nullptr, 0));
      t.small = small != 0;
      t.dark = dark != 0;
      if (!s || t.label.size() > 47) Fail("bad line in art.txt: " + line);
      std::string key;
      if (s >> key) t.key = uint32_t(std::stoul(key, nullptr, 0));
      if (t.key % 4096 != 0 || (t.key && t.small)) Fail("bad key offset in art.txt: " + line);
      list.push_back(std::move(t));
    } else if (kind == "paste") {
      Paste p;
      s >> p.ctx >> p.x >> p.y >> p.w >> p.h >> p.ax >> p.ay;
      if (!s || list.empty()) Fail("bad line in art.txt: " + line);
      list.back().pastes.push_back(p);
    } else {
      Fail("bad line in art.txt: " + line);
    }
  }
  if (contexts < 1 || contexts > 7 || list.empty()) Fail("art.txt is incomplete");
  return list;
}

// ---- game textures ---------------------------------------------------------------
// A peg file: u16 texture count at 0x10, then 0x48-byte entries from 0x18:
// u32 data offset, u16 width, u16 height, u16 format, ..., name at +0x16.
std::string PegTexture(const std::string& peg, const std::string& name, const TextureSpec& t) {
  const auto* d = reinterpret_cast<const uint8_t*>(peg.data());
  if (peg.size() < 0x18) Fail(t.file + " is too small");
  const unsigned count = Be16(d + 0x10);
  int want_index = -1;
  if (!name.empty() && name[0] == '#') want_index = std::stoi(name.substr(1));
  for (unsigned j = 0; j < count; ++j) {
    const size_t e = 0x18 + size_t(j) * 0x48;
    if (e + 0x48 > peg.size()) break;
    std::string nm(reinterpret_cast<const char*>(d + e + 0x16), 0x30);
    nm = nm.substr(0, nm.find('\0'));
    while (!nm.empty() && nm.front() >= 1 && nm.front() < 32) nm.erase(nm.begin());
    while (!nm.empty() && nm.back() >= 1 && nm.back() < 32) nm.pop_back();
    if (want_index >= 0 ? int(j) != want_index : nm != name) continue;
    const uint32_t off = Be32(d + e);
    const int w = Be16(d + e + 4), h = Be16(d + e + 6), fmt = Be16(d + e + 8);
    if (w != t.w || h != t.h || uint32_t(fmt) != t.fmt)
      Fail(t.label + ": texture has a different size or format than expected (unsupported game version?)");
    if (off >= peg.size()) Fail(t.label + ": texture data missing");
    return peg.substr(off, std::min<size_t>(t.nbytes, peg.size() - off));
  }
  Fail(t.label + ": texture " + name + " not found in " + t.file);
}

// ---- Xenos tiling and DXT ----------------------------------------------------------
uint64_t Tiled(uint64_t x, uint64_t y, uint64_t w, unsigned lb) {
  const uint64_t aw = (w + 31) & ~uint64_t(31);
  const uint64_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint64_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint64_t off = macro + ((micro & ~uint64_t(15)) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~uint64_t(511)) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

void Swap16(uint8_t* b, size_t n) {
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(b[i], b[i + 1]);
}

std::array<int, 3> C565(unsigned c) {
  return {int(((c >> 11) & 31) * 255 / 31), int(((c >> 5) & 63) * 255 / 63), int((c & 31) * 255 / 31)};
}

Image Decode(const std::string& data, int w, int h, uint32_t fmt) {
  Image img;
  img.w = w;
  img.h = h;
  img.px.assign(size_t(w) * h * 4, 0);
  const int bw = std::max(1, w / 4), bh = std::max(1, h / 4);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const uint64_t o = Tiled(bx, by, bw, 4) * 16;
      if (o + 16 > data.size()) Fail("texture data too short");
      uint8_t b[16];
      std::memcpy(b, data.data() + o, 16);
      Swap16(b, 16);
      const unsigned c0 = b[8] | (b[9] << 8), c1 = b[10] | (b[11] << 8);
      const uint32_t idx = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
      std::array<std::array<int, 3>, 4> p;
      p[0] = C565(c0);
      p[1] = C565(c1);
      for (int k = 0; k < 3; ++k) {
        p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
        p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
      }
      int al[16];
      if (fmt == 0x191) {
        uint64_t a = 0;
        for (int i = 0; i < 8; ++i) a |= uint64_t(b[i]) << (8 * i);
        for (int i = 0; i < 16; ++i) al[i] = int((a >> (4 * i)) & 15) * 17;
      } else {
        const int a0 = b[0], a1 = b[1];
        uint64_t alpha_bits = 0;
        for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(b[2 + i]) << (8 * i);
        int pal[8] = {a0, a1};
        if (a0 > a1) {
          for (int i = 0; i < 6; ++i) pal[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7;
        } else {
          for (int i = 0; i < 4; ++i) pal[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5;
          pal[6] = 0;
          pal[7] = 255;
        }
        for (int i = 0; i < 16; ++i) al[i] = pal[(alpha_bits >> (3 * i)) & 7];
      }
      for (int i = 0; i < 16; ++i) {
        const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
        if (x >= w || y >= h) continue;
        uint8_t* q = img.At(x, y);
        const auto& c = p[(idx >> (2 * i)) & 3];
        q[0] = uint8_t(c[0]);
        q[1] = uint8_t(c[1]);
        q[2] = uint8_t(c[2]);
        q[3] = uint8_t(al[i]);
      }
    }
  return img;
}

// Principal axis of a 3x3 symmetric matrix (eigenvector of the largest
// eigenvalue), by Jacobi rotations.
std::array<double, 3> PrincipalAxis(double a[3][3]) {
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 64; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-30) break;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::fabs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
        const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
        const double c = 1 / std::sqrt(t * t + 1), s = t * c;
        for (int k = 0; k < 3; ++k) {  // A = A * J
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {  // A = J^T * A
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  // Largest eigenvalue; on a tie the later one (LAPACK sorts ascending and
  // make_art.py takes the last).
  int best = 0;
  for (int k = 1; k < 3; ++k)
    if (a[k][k] >= a[best][best]) best = k;
  return {v[0][best], v[1][best], v[2][best]};
}

unsigned To565(const double c[3]) {
  const int r = int(c[0]), g = int(c[1]), b = int(c[2]);
  return unsigned(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

// Colour half of a DXT3/DXT5 block (8 bytes, little-endian as the PC order).
void ColorBlock(const uint8_t px[16][4], uint8_t out[8]) {
  double rgb[16][3];
  for (int i = 0; i < 16; ++i)
    for (int k = 0; k < 3; ++k) rgb[i][k] = px[i][k];
  int sel[16], n = 0;
  for (int i = 0; i < 16; ++i)
    if (px[i][3] > 8) sel[n++] = i;
  if (n == 0)
    for (int i = 0; i < 16; ++i) sel[n++] = i;
  double mean[3] = {0, 0, 0};
  for (int i = 0; i < n; ++i)
    for (int k = 0; k < 3; ++k) mean[k] += rgb[sel[i]][k];
  for (int k = 0; k < 3; ++k) mean[k] /= n;
  std::array<double, 3> ev;
  if (n > 1) {
    double cen[16][3], m2[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < 3; ++k) cen[i][k] = rgb[sel[i]][k] - mean[k];
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < 3; ++k) m2[k] += cen[i][k];
    for (int k = 0; k < 3; ++k) m2[k] /= n;
    double cov[3][3];
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) {
        double s = 0;
        for (int i = 0; i < n; ++i) s += (cen[i][r] - m2[r]) * (cen[i][c] - m2[c]);
        cov[r][c] = s / (n - 1);
      }
    ev = PrincipalAxis(cov);
  } else {
    ev = {0, 0, 1};
  }
  double lo_p = 0, hi_p = 0;
  for (int i = 0; i < n; ++i) {
    double pr = 0;
    for (int k = 0; k < 3; ++k) pr += (rgb[sel[i]][k] - mean[k]) * ev[k];
    if (i == 0 || pr < lo_p) lo_p = pr;
    if (i == 0 || pr > hi_p) hi_p = pr;
  }
  double lo[3], hi[3];
  for (int k = 0; k < 3; ++k) {
    lo[k] = std::clamp(mean[k] + ev[k] * lo_p, 0.0, 255.0);
    hi[k] = std::clamp(mean[k] + ev[k] * hi_p, 0.0, 255.0);
  }
  unsigned c0 = To565(hi), c1 = To565(lo);
  if (c0 < c1) std::swap(c0, c1);
  double pal[4][3];
  const auto p0 = C565(c0), p1 = C565(c1);
  for (int k = 0; k < 3; ++k) {
    pal[0][k] = p0[k];
    pal[1][k] = p1[k];
    pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
    pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
  }
  uint32_t idx = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    double best_d = 0;
    for (int j = 0; j < 4; ++j) {
      const double d0 = rgb[i][0] - pal[j][0], d1 = rgb[i][1] - pal[j][1], d2 = rgb[i][2] - pal[j][2];
      const double d = (d0 * d0 + d1 * d1) + d2 * d2;
      if (j == 0 || d < best_d) best = j, best_d = d;
    }
    idx |= uint32_t(best) << (2 * i);
  }
  out[0] = uint8_t(c0);
  out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1);
  out[3] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t(idx >> (8 * i));
}

void Dxt3Block(const uint8_t px[16][4], uint8_t out[16]) {
  uint64_t alpha = 0;
  for (int i = 0; i < 16; ++i) {
    const double a = std::nearbyint(px[i][3] / 17.0);  // round half to even, as numpy
    alpha |= uint64_t(std::clamp(int(a), 0, 15)) << (4 * i);
  }
  for (int i = 0; i < 8; ++i) out[i] = uint8_t(alpha >> (8 * i));
  ColorBlock(px, out + 8);
}

void Dxt5Block(const uint8_t px[16][4], uint8_t out[16]) {
  int a0 = 0, a1 = 255;
  for (int i = 0; i < 16; ++i) a0 = std::max<int>(a0, px[i][3]), a1 = std::min<int>(a1, px[i][3]);
  if (a0 == a1) {
    a1 = a0 > 0 ? std::max(0, a0 - 1) : 0;
    a0 = std::max(a0, a1 + 1);
  }
  int pal[8] = {a0, a1};
  for (int i = 1; i < 7; ++i) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
  uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    for (int j = 1; j < 8; ++j)
      if (std::abs(pal[j] - px[i][3]) < std::abs(pal[best] - px[i][3])) best = j;
    bits |= uint64_t(best) << (3 * i);
  }
  out[0] = uint8_t(a0);
  out[1] = uint8_t(a1);
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(bits >> (8 * i));
  ColorBlock(px, out + 8);
}

void Put32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(char(v >> (8 * i)));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: glyphgen <game packfiles folder> <art.txt> <output kbm_ui.bin>\n");
    return 2;
  }
  const fs::path packdir = argv[1], list_path = argv[2], out_path = argv[3];
  int contexts = 0;
  const std::vector<TextureSpec> list = LoadList(list_path, contexts);
  const Image art = LoadPng(list_path.parent_path() / "art.png");

  std::map<std::string, wml::Packfile> packs;
  std::map<std::string, std::string> files;
  auto file_of = [&](const TextureSpec& t) -> const std::string& {
    const std::string key = t.pack + "/" + t.file;
    auto it = files.find(key);
    if (it != files.end()) return it->second;
    auto& pack = packs[t.pack];
    if (pack.Names().empty()) {
      std::string error;
      if (!pack.Load(packdir / t.pack, &error)) Fail(t.pack + ": " + error);
    }
    std::string data;
    if (!pack.Read(t.file, data)) Fail(t.file + " not found in " + t.pack);
    return files[key] = std::move(data);
  };

  std::string out = "SRG3";
  Put32(out, uint32_t(list.size()));
  Put32(out, uint32_t(contexts + 1));
  for (const TextureSpec& t : list) {
    if (t.fmt != 0x191 && t.fmt != 0x192) Fail(t.label + ": unsupported texture format");
    const std::string tex = PegTexture(file_of(t), t.tex, t);
    if (tex.size() < size_t(t.key) + 4096) Fail(t.label + ": texture too small");
    const Image orig = Decode(tex, t.w, t.h, t.fmt);

    std::vector<Image> versions;
    std::set<std::pair<int, int>> block_set;
    for (int ctx = 0; ctx < contexts; ++ctx) {
      Image a = orig;
      for (const Paste& p : t.pastes) {
        if (p.ctx != ctx) continue;
        if (p.x < 0 || p.y < 0 || p.x + p.w > t.w || p.y + p.h > t.h || p.ax + p.w > art.w || p.ay + p.h > art.h)
          Fail(t.label + ": picture outside the texture");
        for (int y = 0; y < p.h; ++y)
          for (int x = 0; x < p.w; ++x) {
            uint8_t* d = a.At(p.x + x, p.y + y);
            std::memcpy(d, art.At(p.ax + x, p.ay + y), 4);
            if (t.dark) d[0] = d[1] = d[2] = 0;  // outline/shadow font layers
          }
        for (int by = p.y / 4; by <= (p.y + p.h - 1) / 4; ++by)
          for (int bx = p.x / 4; bx <= (p.x + p.w - 1) / 4; ++bx) block_set.insert({bx, by});
      }
      versions.push_back(std::move(a));
    }
    const std::vector<std::pair<int, int>> blocks(block_set.begin(), block_set.end());  // sorted by x, then y
    std::vector<uint32_t> offs;
    for (auto [bx, by] : blocks) offs.push_back(uint32_t(Tiled(bx, by, t.w / 4, 4) * 16));
    for (uint32_t o : offs)
      if (o + 16 > tex.size()) Fail(t.label + ": block outside the texture");

    const uint32_t size = t.small ? *std::max_element(offs.begin(), offs.end()) + 16 : uint32_t(tex.size());
    std::string label = t.label;
    label.resize(48, '\0');
    out += label;
    Put32(out, size);
    Put32(out, uint32_t(offs.size()));
    Put32(out, (t.small ? 1 : 0) | (t.key ? 2 : 0));
    if (t.key) Put32(out, t.key);
    out.append(tex.data() + t.key, 4096);
    for (uint32_t o : offs) Put32(out, o);
    for (uint32_t o : offs) out.append(tex.data() + o, 16);  // version 0: the original
    for (const Image& a : versions)
      for (auto [bx, by] : blocks) {
        uint8_t px[16][4], enc[16];
        for (int i = 0; i < 16; ++i) std::memcpy(px[i], a.At(bx * 4 + i % 4, by * 4 + i / 4), 4);
        if (t.fmt == 0x191) Dxt3Block(px, enc);
        else Dxt5Block(px, enc);
        Swap16(enc, 16);
        out.append(reinterpret_cast<const char*>(enc), 16);
      }
  }

  fs::path tmp = out_path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) Fail("cannot write " + tmp.string());
    f.write(out.data(), std::streamsize(out.size()));
    if (!f) Fail("cannot write " + tmp.string());
  }
  std::error_code ec;
  fs::rename(tmp, out_path, ec);
  if (ec) Fail("cannot write " + out_path.string() + ": " + ec.message());
  std::printf("glyphgen: wrote %s (%zu textures, %zu bytes)\n", out_path.string().c_str(), list.size(), out.size());
  return 0;
}
