# How it works

## Static recompilation

The Xbox 360 runs PowerPC code. Instead of emulating that CPU at runtime, the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) translates every function
in the game's executable (`default.xex`) to C++ ahead of time. The result is
compiled with Clang into a normal x86-64 Windows program. The SDK also provides
the rest of the console: the kernel and system libraries, the Xenos GPU
(translated to Direct3D 12), XMA audio and controller input.

Saints Row needs some game-specific help on top of that. This document lists
what was changed and why.

## Recompiler configuration

`config/saintsrow_manifest.toml`:

- **ABI helpers.** Addresses of the compiler's register save/restore routines,
  which the recompiler needs in order to translate function prologues and
  epilogues.
- **Extra functions.** Nineteen small functions (mostly C++ adjustor thunks and
  virtual-call forwarders) are only reached through vtables, so static analysis
  does not find them. They are listed explicitly; without them, those virtual
  calls do nothing.
- **Mid-function hooks.** Two places inside functions get a short C++ hook:
  a voice-list loop in the audio code that can hit empty (null) slots, and a
  streaming routine whose byte-sized loop bounds are kept within 8 bits.

## Game code overrides

`project/src/stubs.cpp` replaces some recompiled functions with C++ versions
that call the original where appropriate:

- **Rendering.** Saints Row uses the Xbox 360's predicated tiling: it records
  commands once and replays them per screen tile through nested indirect
  buffers, synchronised with interrupts and CPU-written fences. The overrides
  keep the game's tiling executor, fence handling and command-buffer
  submission consistent with how the SDK processes GPU commands, and guard
  against command buffers whose memory has already been reused.
- **Audio.** The XAudio engine is created with a bounded number of worker
  threads, and the loader no longer waits forever for sounds whose playback
  state never reports completion.
- **Kernel objects.** A few semaphore, event and thread-creation calls are
  adjusted for differences between the SDK and real hardware.
- **Null-pointer paths.** Several string and lookup routines are regularly
  called with null pointers. On the console those reads are harmless; on PC each
  one would raise an exception. Guards return the same result without the
  fault.
- **System.** Sign-in and content-licence queries report a signed-in local
  profile and the full game.

`project/src/main.cpp` sets up guest memory (including the low "null page" the
game expects to be readable), creates the window, loads the GPU backend and
starts the game.

## SDK changes

`patches/rexglue-sdk.patch`:

- **Nested indirect buffers.** Indirect buffers were read through a ring buffer
  exactly as large as the data, so its write position wrapped to zero and every
  nested buffer executed nothing. Saints Row draws most of its scene through
  them. The fix gives the reader one spare dword.
- **Waiting on memory.** When the GPU waits for a value written by a guest
  interrupt handler, the command processor now runs queued work while waiting,
  and gives up after a time limit instead of deadlocking.
- **Robustness.** Nested indirect buffers are limited in depth and skipped when
  they do not contain plausible GPU commands; unknown or broken packets are
  skipped instead of aborting the rest of a buffer.
- **Guest data source.** GPU data written only through the CPU's virtual
  mapping is read from there when the physical view is empty.
- **Rendering.** Unbound textures sample as white instead of black; draws
  without a finished pipeline are skipped; zero-scale viewports and 4-vertex
  resolve rectangles are handled.
- **Audio.** Audio client registration no longer holds a global lock while
  creating the audio driver (which could deadlock) and no longer hands out
  audio buffers before the client is fully registered.
- **Calls to unknown functions** return 0 instead of terminating the program.
- **Input** is ignored while the window is not focused.
