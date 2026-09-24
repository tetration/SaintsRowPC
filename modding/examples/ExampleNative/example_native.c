/*
 * Example code mod for Whompay's Mod Loader.
 *
 * Hooks the game's present function (called once per frame) to count frames,
 * and logs the frame rate when F9 is pressed.
 */
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wml.h"

static const WmlApi* api;
static const WmlMod* self;
static WmlGuestFunction original_present;

static unsigned long frames;
static unsigned long frames_at_last_report;
static ULONGLONG time_at_last_report;

/* Runs instead of the game's present function; calls the original. */
static void present_hook(WmlContext* ctx, uint8_t* base) {
  frames++;
  original_present(ctx, base);
}

static void on_frame(void* user) {
  (void)user;
  if (api->key_pressed(VK_F9)) {
    ULONGLONG now = GetTickCount64();
    double seconds = (now - time_at_last_report) / 1000.0;
    char text[128];
    snprintf(text, sizeof(text), "%lu frames in %.1f s (%.1f fps)",
             frames - frames_at_last_report, seconds,
             seconds > 0 ? (frames - frames_at_last_report) / seconds : 0.0);
    api->log(self, text);
    frames_at_last_report = frames;
    time_at_last_report = now;
  }
}

WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) {
    return 1; /* loader too old */
  }
  if (api->hook(0x825E54A8, present_hook, &original_present) != 0) {
    return 2;
  }
  api->on_frame(on_frame, NULL);
  time_at_last_report = GetTickCount64();
  api->log(self, "Hello from C! Press F9 in game to log the frame rate.");
  return 0;
}
