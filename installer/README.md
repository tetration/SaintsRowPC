# Setup and Updater

`SaintsRowPC-Setup.exe` builds the game on the player's own PC from their
own disc image. It:

1. installs anything missing: Git (portable MinGit), Visual Studio 2022 Build
   Tools with the C++ and Clang components, and the Visual C++ runtime;
2. downloads this repository;
3. runs `scripts\setup.ps1` with the chosen disc image;
4. creates Start menu and desktop shortcuts.

`SaintsRowPC-Updater.exe` is the same program starting in update mode (also
`SaintsRowPC-Setup.exe /update`). It brings an existing folder, a git clone or
a zip download, to the latest version and runs `scripts\setup.ps1` again, which
rebuilds only what changed. `build`, `dist` and `mods\modlist.ini` are kept.

Neither contains game code. Build both with `installer\build.ps1` and attach
`installer\out\*.exe` to a GitHub release.
