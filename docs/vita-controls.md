# PS Vita controls

| Vita | Game |
|---|---|
| D-pad, left stick | move |
| Cross | button 1: attack (confirms in menus) |
| Circle | button 2: jump |
| Square or L | button 3 |
| START | coin, then 1P start (one press) |
| SELECT (short press, alone) | pause / resume |
| SELECT + L | save state |
| SELECT + R | load state |
| SELECT + START, held 2 s | delete the save state |
| SELECT, held at launch | start fresh, without resuming |

## Saves

- `ux0:data/mystwarr/state.bin`: the save state. It is also written every 3 minutes of play and resumed at launch, after a few seconds of black
  screen while the game boots. A save or load takes effect at the game's next idle point, normally within 2 frames, and a save or load
  while paused also resumes the game. A bar in the corner of the screen confirms: green saved, blue loaded, red failed or deleted.
- `ux0:data/mystwarr/eeprom.nv`: settings and high scores, as on the board's EEPROM. Save states leave it alone, so loading one never
  replaces newer scores.

## Option file

An empty file `ux0:data/mystwarr/aspect43` shows the picture in 4:3 instead of full screen.
