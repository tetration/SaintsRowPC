// Keyboard and mouse controls, laid out like Saints Row 2 on PC.
//
// The game only knows the Xbox 360 controller, so keys are turned into
// controller input on top of whatever a real controller reports (a real
// controller works exactly as before). Some keys mean different things on
// foot and in a vehicle, the way SR2 does it (W walks forward on foot and
// accelerates in a car); the game's own "is the player in a vehicle" check is
// read from memory to tell which. The mouse turns the camera directly (see
// the camera hook at the end), and picks items in the radial menu.
//
// On foot                          In a vehicle
//   WASD      move                   W / S     accelerate / brake, reverse
//   Mouse     camera                 A / D     steer
//   LMB       primary attack         LMB       drive-by attack
//   RMB       secondary attack       Space     handbrake
//   Space     jump                   Shift     nitrous
//   Shift     sprint                 Ctrl      hydraulics
//   E         action / enter car     E         exit car
//   R         reload / grab weapon   Z / C     look left / right
//   F         kick                   X         look back
//   C         crouch
//   MMB / V   right stick click
// Everywhere
//   Q (hold)  radial menu (point at an item with the mouse or WASD)
//   Esc / M   pause / map            Tab       back
//   Arrows    D-pad (recruit, cancel activity, radio / audio track, taunt)
//   Enter     A (menus)              Backspace B (menus)

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include "kbm.h"
#include "wml/mod_loader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/input/input.h>
#include <rex/ppc/function.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rex::kernel::xam {
extern uint32_t XamInputGetState_entry(uint32_t user_index, uint32_t flags,
                                       ppc_ptr_t<rex::input::X_INPUT_STATE> input_state);
}

namespace {

using namespace rex::input;

#ifdef _WIN32
bool GameHasFocus() {
  HWND foreground = GetForegroundWindow();
  if (!foreground) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(foreground, &pid);
  return pid == GetCurrentProcessId();
}

// GetAsyncKeyState is a system call and the game polls the controller many
// times per frame, so the keys are read at most every 2 ms.
bool g_key_down[256];
std::chrono::steady_clock::time_point g_keys_read{};

void RefreshKeys() {
  auto now = std::chrono::steady_clock::now();
  if (now - g_keys_read < std::chrono::milliseconds(2)) return;
  g_keys_read = now;
  static const int kKeys[] = {'Q', 'E', 'R', 'F', 'C', 'V', 'M', 'W', 'A', 'S', 'D', 'Z', 'X',
                              VK_ESCAPE, VK_TAB, VK_RETURN, VK_BACK, VK_UP, VK_DOWN, VK_LEFT,
                              VK_RIGHT, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_SPACE, VK_SHIFT,
                              VK_CONTROL};
  for (int vk : kKeys) g_key_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Down(int vk) { return (g_key_down[vk & 0xFF] && !wml::KeyTaken(vk)) || wml::KeyForced(vk); }
#endif

std::atomic<double> g_sensitivity{1.0};

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
//
// While the game has focus the cursor is held in the middle of the window and
// its movement is collected here. The camera takes it directly (see the
// sub_8210D518 hook below); only if that camera isn't running (other camera
// modes) is the movement turned into right stick input instead.

struct MouseState {
  bool captured = false;
  double dx = 0, dy = 0;  // movement not yet used by the camera
  std::chrono::steady_clock::time_point last_poll{};
  std::chrono::steady_clock::time_point last_camera{};
  double vx = 0, vy = 0;  // smoothed speed for the stick fallback
  // Radial menu: where the mouse points, relative to where it was opened.
  bool radial_open = false;
  double radial_x = 0, radial_y = 0;
};
MouseState g_mouse;
std::mutex g_mouse_mutex;

#ifdef _WIN32
void ReleaseMouseLocked() {
  if (g_mouse.captured) {
    ClipCursor(nullptr);
    g_mouse.captured = false;
  }
  g_mouse.dx = g_mouse.dy = 0;
  g_mouse.vx = g_mouse.vy = 0;
}

// Collects cursor movement since the last call and puts the cursor back in the
// middle of the game window. Returns the movement in pixels.
void PollMouseLocked(double& dx, double& dy) {
  dx = dy = 0;
  HWND window = GetForegroundWindow();
  RECT client;
  if (!window || !GetClientRect(window, &client)) {
    ReleaseMouseLocked();
    return;
  }
  POINT top_left{client.left, client.top}, bottom_right{client.right, client.bottom};
  ClientToScreen(window, &top_left);
  ClientToScreen(window, &bottom_right);
  RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  POINT center{(screen.left + screen.right) / 2, (screen.top + screen.bottom) / 2};
  POINT cursor;
  GetCursorPos(&cursor);
  ClipCursor(&screen);  // cheap, and follows window moves / resizes
  if (!g_mouse.captured) {
    g_mouse.captured = true;
    SetCursorPos(center.x, center.y);
    return;
  }
  dx = double(cursor.x - center.x);
  dy = double(cursor.y - center.y);
  if (dx != 0 || dy != 0) SetCursorPos(center.x, center.y);
}
#endif

// Right stick position for mouse speed (only used when the camera hook isn't
// running): past the stick dead zone, then proportional to speed.
void MouseToStickLocked(double dx, double dy, double dt, int& rx, int& ry) {
  dt = std::clamp(dt, 0.001, 0.1);
  double a = 1.0 - std::exp(-dt / 0.03);
  g_mouse.vx += (dx / dt - g_mouse.vx) * a;
  g_mouse.vy += (dy / dt - g_mouse.vy) * a;
  const double sensitivity = g_sensitivity.load(std::memory_order_relaxed);
  auto to_stick = [&](double v) -> int {
    double speed = std::fabs(v) * sensitivity;
    if (speed < 8.0) return 0;
    constexpr double kDeadZone = 8700.0;
    double m = kDeadZone + (32767.0 - kDeadZone) * std::min(1.0, speed / 800.0);
    return int(v < 0 ? -m : m);
  };
  rx = to_stick(g_mouse.vx);
  ry = to_stick(-g_mouse.vy);
}

float LoadF32(uint8_t* base, uint32_t address) {
  uint32_t bits = PPC_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
void StoreF32(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  PPC_STORE_U32(address, bits);
}

// The game's own test (script function is_player_in_vehicle, 0x824D0A50 ->
// sub_82448010), done with plain memory reads.
bool PlayerInVehicle(uint8_t* base) {
  const uint32_t player = PPC_LOAD_U32(0x8309ABECu);
  if (!player) return false;
  if (PPC_LOAD_U32(player + 3456) != 0) return false;
  const uint32_t vehicle_handle = PPC_LOAD_U32(player + 2496);
  // sub_82448590: getting in / out and similar transitions count as on foot.
  if (vehicle_handle && (PPC_LOAD_U8(player + 2569) & 0x10)) return false;
  const uint32_t state = PPC_LOAD_U32(player + 508);
  if (state == 9 || state == 12) return false;
  // sub_82569630: the handle must name a live vehicle object (type 5).
  if (!vehicle_handle) return false;
  const uint32_t index = vehicle_handle & 0xFFFF;
  if (index >= 4096) return false;
  const uint32_t object = PPC_LOAD_U32(0x830866C8u + 12 + index * 16);
  if (!object) return false;
  return PPC_LOAD_U32(object + 68) == vehicle_handle && PPC_LOAD_U32(object + 72) == 5;
}

struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int lx = 0, ly = 0;
};

#ifdef _WIN32
Pad ReadKeyboard(uint8_t* base) {
  Pad p;
  const bool in_vehicle = PlayerInVehicle(base);
  auto press = [&](bool down, uint16_t button) {
    if (down) p.buttons |= button;
  };

  // Everywhere.
  press(Down('Q'), X_INPUT_GAMEPAD_B);
  press(Down(VK_ESCAPE) || Down('M'), X_INPUT_GAMEPAD_START);
  press(Down(VK_TAB), X_INPUT_GAMEPAD_BACK);
  press(Down(VK_RETURN), X_INPUT_GAMEPAD_A);
  press(Down(VK_BACK), X_INPUT_GAMEPAD_B);
  press(Down(VK_UP), X_INPUT_GAMEPAD_DPAD_UP);
  press(Down(VK_DOWN), X_INPUT_GAMEPAD_DPAD_DOWN);
  press(Down(VK_LEFT), X_INPUT_GAMEPAD_DPAD_LEFT);
  press(Down(VK_RIGHT), X_INPUT_GAMEPAD_DPAD_RIGHT);
  press(Down('E'), X_INPUT_GAMEPAD_Y);
  if (Down(VK_LBUTTON)) p.rt = 0xFF;
  press(Down(VK_MBUTTON) || Down('V'), X_INPUT_GAMEPAD_RIGHT_THUMB);

  const int left = Down('A') ? 1 : 0, right = Down('D') ? 1 : 0;
  p.lx = (right - left) * 32767;

  if (in_vehicle) {
    press(Down('W'), X_INPUT_GAMEPAD_A);  // accelerate
    press(Down('S'), X_INPUT_GAMEPAD_X);  // brake / reverse
    if (Down(VK_SPACE)) p.lt = 0xFF;       // handbrake
    press(Down(VK_SHIFT), X_INPUT_GAMEPAD_RIGHT_THUMB);  // nitrous
    press(Down(VK_CONTROL), X_INPUT_GAMEPAD_LEFT_THUMB);  // hydraulics
    press(Down('Z') || Down('X'), X_INPUT_GAMEPAD_LEFT_SHOULDER);   // look left
    press(Down('C') || Down('X'), X_INPUT_GAMEPAD_RIGHT_SHOULDER);  // look right
  } else {
    const int up = Down('W') ? 1 : 0, down = Down('S') ? 1 : 0;
    p.ly = (up - down) * 32767;
    if (Down(VK_RBUTTON)) p.lt = 0xFF;                            // secondary attack
    press(Down(VK_SPACE), X_INPUT_GAMEPAD_X);                     // jump
    press(Down(VK_SHIFT), X_INPUT_GAMEPAD_RIGHT_SHOULDER);        // sprint
    press(Down('R'), X_INPUT_GAMEPAD_A);                          // reload / grab weapon
    press(Down('F'), X_INPUT_GAMEPAD_LEFT_SHOULDER);              // kick
    press(Down('C'), X_INPUT_GAMEPAD_LEFT_THUMB);                 // crouch
  }
  return p;
}
#endif

}  // namespace

void sr::SetMouseSensitivity(double sensitivity) {
  g_sensitivity.store(sensitivity, std::memory_order_relaxed);
}

// Adds the keyboard and mouse to the controller state the game reads.
PPC_FUNC_IMPL(__imp__XamInputGetState) {
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t state_addr = ctx.r5.u32;
  HostToGuestFunction<rex::kernel::xam::XamInputGetState_entry>(ctx, base);
#ifdef _WIN32
  if (!state_addr || ctx.r3.u32 != 0) return;
  if ((user_index & 0xFF) != 0 && (user_index & 0xFF) != 0xFF) return;
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  if (!GameHasFocus()) {
    ReleaseMouseLocked();
    return;
  }
  RefreshKeys();
  Pad k = ReadKeyboard(base);

  auto now = std::chrono::steady_clock::now();
  double poll_dt = std::chrono::duration<double>(now - g_mouse.last_poll).count();
  g_mouse.last_poll = now;
  double dx, dy;
  PollMouseLocked(dx, dy);

  int rx = 0, ry = 0;
  const bool radial = Down('Q');
  if (radial) {
    // Radial menu: point at an item with the mouse, like Saints Row 2.
    if (!g_mouse.radial_open) {
      g_mouse.radial_open = true;
      g_mouse.radial_x = g_mouse.radial_y = 0;
    }
    g_mouse.radial_x += dx;
    g_mouse.radial_y += dy;
    const double len = std::hypot(g_mouse.radial_x, g_mouse.radial_y);
    constexpr double kRadius = 120.0;
    if (len > kRadius) {
      g_mouse.radial_x *= kRadius / len;
      g_mouse.radial_y *= kRadius / len;
    }
    if (len > 25.0 && !k.lx && !k.ly) {
      k.lx = int(g_mouse.radial_x / std::max(len, 1.0) * 32767.0);
      k.ly = int(-g_mouse.radial_y / std::max(len, 1.0) * 32767.0);
    }
    g_mouse.dx = g_mouse.dy = 0;  // the camera stays still meanwhile
  } else {
    g_mouse.radial_open = false;
    g_mouse.dx += dx;
    g_mouse.dy += dy;
    // Fallback for cameras that don't go through the hook below.
    double since_camera = std::chrono::duration<double>(now - g_mouse.last_camera).count();
    if (since_camera > 0.25) {
      MouseToStickLocked(g_mouse.dx, g_mouse.dy, poll_dt, rx, ry);
      g_mouse.dx = g_mouse.dy = 0;
    }
  }

  if (!k.buttons && !k.lt && !k.rt && !k.lx && !k.ly && !rx && !ry) return;

  auto* state = reinterpret_cast<X_INPUT_STATE*>(base + state_addr);
  auto& pad = state->gamepad;
  pad.buttons = uint16_t(pad.buttons) | k.buttons;
  if (k.lt > pad.left_trigger) pad.left_trigger = k.lt;
  if (k.rt > pad.right_trigger) pad.right_trigger = k.rt;
  if (k.lx) pad.thumb_lx = int16_t(std::clamp(k.lx, -32767, 32767));
  if (k.ly) pad.thumb_ly = int16_t(std::clamp(k.ly, -32767, 32767));
  if (rx) pad.thumb_rx = int16_t(rx);
  if (ry) pad.thumb_ry = int16_t(ry);
  // The game ignores a state whose packet number hasn't changed.
  state->packet_number = uint32_t(state->packet_number) + 1;
#endif
}

// ---------------------------------------------------------------------------
// Mouse look
// ---------------------------------------------------------------------------
//
// sub_8210D518 updates the third-person camera once per frame (f1 = frame
// time). Its camera state is at 0x827D9778: +320 is the horizontal and +336
// the vertical turn input, which the player controls fill in from the right
// stick. The turn code (sub_8210CFE0 / sub_8210C9E0, tuned by
// camera_free.xtbl) uses a "slow pan" zone that turns at input x multiplier,
// immediately, and a "fast pan" zone with acceleration for a pegged stick.
// For the mouse the fast zone is switched off and the slow multipliers set to
// 1 for the duration of the call, so the camera turns exactly by the mouse
// movement of this frame.
extern "C" void __imp__sub_8210D518(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8210D518) {
  constexpr uint32_t kCamera = 0x827D9778;
  constexpr uint32_t kSlowPanH = 0x827D9348, kSlowPanV = 0x827D934C;
  constexpr uint32_t kFastPanThreshold = 0x827D9350;
  constexpr uint32_t kInvertFlags = 0x829B83F2;  // bit 0x40 invert X, 0x20 invert Y

  double dx = 0, dy = 0;
  {
    std::lock_guard<std::mutex> lock(g_mouse_mutex);
    g_mouse.last_camera = std::chrono::steady_clock::now();
    dx = g_mouse.dx;
    dy = g_mouse.dy;
    g_mouse.dx = g_mouse.dy = 0;
  }
  const double dt = ctx.f1.f64;
  const double mod_turn = wml::TakeCameraTurn();
  if ((dx == 0 && dy == 0 && mod_turn == 0) || !(dt > 0.0001) || dt > 0.5) {
    __imp__sub_8210D518(ctx, base);
    return;
  }

  // Radians per pixel at sensitivity 1.
  constexpr double kRadiansPerPixel = 0.001;
  const double k = kRadiansPerPixel * g_sensitivity.load(std::memory_order_relaxed) / dt;
  const uint8_t invert = PPC_LOAD_U8(kInvertFlags);
  double yaw = dx * k, pitch = -dy * k;
  if (invert & 0x40) yaw = -yaw;
  if (invert & 0x20) pitch = -pitch;
  yaw += mod_turn / dt;

  const float slow_h = LoadF32(base, kSlowPanH), slow_v = LoadF32(base, kSlowPanV);
  const float threshold = LoadF32(base, kFastPanThreshold);
  StoreF32(base, kSlowPanH, 1.0f);
  StoreF32(base, kSlowPanV, 1.0f);
  StoreF32(base, kFastPanThreshold, 1.0e6f);
  StoreF32(base, kCamera + 320, float(yaw));
  StoreF32(base, kCamera + 324, 0.0f);
  StoreF32(base, kCamera + 336, float(pitch));
  __imp__sub_8210D518(ctx, base);
  StoreF32(base, kSlowPanH, slow_h);
  StoreF32(base, kSlowPanV, slow_v);
  StoreF32(base, kFastPanThreshold, threshold);
}
