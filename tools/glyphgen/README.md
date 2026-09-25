# glyphgen

Makes `dist/kbm_ui.bin`: the keyboard and mouse pictures the game shows in
place of controller buttons (see `project/src/glyphs.cpp`).

The file has to contain parts of the game's own textures, so it is never
stored in this repository. Setup builds `glyphgen` with the game and runs it on
the player's PC:

    glyphgen <dist\game\packfiles> tools\glyphgen\art.txt dist\kbm_ui.bin

It reads the button and font textures from the player's packfiles, pastes the
pictures from `art.png` over the controller buttons for each control context
(menus, on foot, in a vehicle, character creator) and re-encodes the changed
DXT blocks.

- `art.png` holds our own pictures: keycaps, mouse buttons, arrow keys and the
  Q/E plates, all drawn from scratch by `art.py`.
- `art.txt` lists which picture goes where (texture names and rectangles).

## Changing the pictures

Edit `art.py` (the drawings) or `make_art.py` (which key goes where), then
regenerate both files from your own extracted game:

    pip install pillow numpy
    python tools/glyphgen/make_art.py dist/game/packfiles

`--reference out.bin` also writes the result the slow way in Python, to compare
with glyphgen. The pictures use DejaVu Sans Bold (free license); pass `--font`
if it is somewhere else on your system.
