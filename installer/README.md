# Setup

`SaintsRowPC-Setup.exe` builds the game on the player's own PC from their
own disc image. It:

1. installs anything missing: Git (portable MinGit), Visual Studio 2022 Build
   Tools with the C++ and Clang components, and the Visual C++ runtime;
2. downloads this repository;
3. runs `scripts\setup.ps1` with the chosen disc image;
4. creates Start menu and desktop shortcuts.

It contains no game code. Build it with `installer\build.ps1`. Pushing a tag
such as `v1.0.0` builds it on GitHub and attaches it to a release.
