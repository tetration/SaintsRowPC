// Button glyphs that follow the input device (glyphs.cpp).
#pragma once
#include <cstdint>

namespace sr {

enum class GlyphContext : int { kMenu = 0, kOnFoot = 1, kVehicle = 2, kCreator = 3 };

// Called from the input hook every poll: which device was used this poll and
// what the player is doing (the keyboard layout differs per context).
void GlyphsNoteInput(uint8_t* base, bool controller_used, bool kbm_used, GlyphContext context);

}  // namespace sr
