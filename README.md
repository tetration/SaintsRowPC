# Saints Row PC

An unofficial native Windows port of **Saints Row** (Xbox 360, 2006). The game's
PowerPC code is statically recompiled to x86-64 with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), so it runs as a normal
Windows program rather than under an emulator.

**This repository contains no game code or data.** You build the game yourself,
on your own PC, from your own disc.

> Saints Row PC is a fan project. It is not affiliated with, endorsed by or
> sponsored by Volition, THQ Nordic, Deep Silver, Plaion or Microsoft.

## Status

Playable. The intro videos, character creator, missions and free roam all work,
with sound, at the original 30 FPS. The game renders at 2x its original
resolution by default.

### Known issues

- Some textures on the character flicker slightly while rotating them in the
  character creator.
- Green bars can appear at the edges of the "Welcome to Stilwater" splash.
- After "Begin Game" the loading screen can sit for a few extra seconds.
- Only the disc version this port was made with is supported. Setup warns if
  your `default.xex` is different.

## Requirements

- Your own Saints Row (Xbox 360) disc, dumped to an `.iso` file.
- Windows 10 or 11 (64-bit) and a GPU with Direct3D 12 support.
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/)
  (Community or Build Tools, both free) with the **Desktop development with
  C++** workload and these individual components:
  - C++ Clang Compiler for Windows
  - C++ CMake tools for Windows
- [Git for Windows](https://git-scm.com/download/win).
- About 15 GB of free disk space and 16 GB of RAM. The first build takes
  30–90 minutes depending on your CPU.

## Building

1. Download this repository to a short path, for example `C:\SaintsRowPC`.
2. Run `setup.bat`.
3. Select your Saints Row `.iso` when asked.

When it finishes, the game is in the `dist` folder. If a step fails, fix the
cause and run `setup.bat` again; finished steps are skipped. See
[docs/BUILDING.md](docs/BUILDING.md) for what each step does and for
troubleshooting.

## Playing

Run `dist\WhompaysModLoader.exe` to pick mods and play, or `dist\saintsrow.exe`
to play directly.

| Action | Control |
|---|---|
| Toggle fullscreen / window | F11 |
| Game controls | Xbox controller (XInput) |
| Start, A, B | Enter, Space, Backspace |
| Left stick | W A S D |

Input is ignored while the game window is not focused. Saves and profile data
are stored in `dist\game`.

Options, set by creating a file next to `saintsrow.exe`:

| File | Effect |
|---|---|
| `res_scale.txt` | Internal resolution scale: `1` (720p), `2` (default) or `3`. |
| `start_windowed` | Start in a window instead of fullscreen. The file can be empty. |

## Mods

Saints Row PC comes with **Whompay's Mod Loader**. Run
`dist\WhompaysModLoader.exe` to turn mods on or off and change their load
order, then press Play. Mods can replace game files, run Lua scripts, or load
C/C++ code that hooks the game's functions. See
[modding/README.md](modding/README.md) to use or make mods.

## How it works

The ReXGlue SDK translates every PowerPC function in the game's executable to
C++ and reimplements the Xbox 360 kernel, GPU (on Direct3D 12), audio and
input. This repository adds the Saints Row-specific parts:

| Path | Contents |
|---|---|
| `config/saintsrow_manifest.toml` | Recompiler configuration: ABI helpers, functions static analysis misses, mid-function hooks |
| `project/src/stubs.cpp` | Game-specific replacements for recompiled functions and kernel calls |
| `project/src/main.cpp` | Program entry: memory setup, window, runtime |
| `project/src/wml`, `project/launcher` | Whompay's Mod Loader and its launcher |
| `modding` | Mod API header, examples and documentation |
| `patches/rexglue-sdk.patch` | Changes to the SDK that the game needs |
| `tools/xiso_extract` | Xbox disc image (XDVDFS) extractor |
| `scripts/setup.ps1` | The build script behind `setup.bat` |

[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) explains the individual fixes.

## Contributing

Bug reports and fixes are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

This project distributes only original source code, configuration files and a
patch to the BSD-licensed ReXGlue SDK. It does not include, and must not be
used to distribute, any part of Saints Row: no disc images, game files,
recompiled code or built executables. Dump your own disc. Requests for or links
to game files will be removed.

The project's own code is released under the [MIT License](LICENSE). The SDK
and the libraries it downloads during the build are covered by their own
licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). "Saints Row" is
a trademark of its owner and is used here only to name the game this project
works with.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay and
  contributors.
- [Xenia](https://xenia.jp) by Ben Vanik and contributors, which the SDK is
  derived from.
- Volition, for the game.
