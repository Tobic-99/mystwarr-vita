# Mystic Warriors for the PS Vita

A static recompilation of Mystic Warriors (Konami, 1993, GX128 arcade board) to native ARM code. The game's 68000 program is translated
to C++ and linked with a native runtime that replaces the board: memory map, EEPROM, a software renderer (tile layers, sprites, shadows,
blending) and the sound board (Z80 interpreter, two K054539).

The repository contains no game code, data or artwork. You need your own copy of the MAME ROM set `mystwarr`.

This port is intended only for owners of the game. Do not ask how to get the ROMs: piracy is not supported.

The VPK holds the translated code and nothing of the ROMs: graphics, sound and the programs' data are read from your ROM files.

How the port runs on the PS Vita: [docs/optimizations.md](docs/optimizations.md).

## Installing on the PS Vita

You need a PS Vita with HENkaku/Ensō, [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases) and `libshacccg.suprx`
(vitaGL compiles its shaders with it).

1. Install `mystwarr.vpk` from the [releases](../../releases) with VitaShell.
2. Copy your ROM files to `ux0:data/mystwarr/rom/`:
   ```
   128eaa01.20f 128eaa02.20g 128a03.19f 128a04.19g     (68000 program and data)
   128a05.6b    128a06.2d    128a07.1d                 (sound program, samples)
   128a08.1h    128a09.1k    128a10.3h                 (tiles)
   128a11.10k   128a12.12k   128a13.17k
   128a14.19k   128a15.20k   128a16.22k                (sprites)
   mystwarr.nv                                         (default EEPROM)
   ```
3. Start Mystic Warriors.

[Controls and saves](docs/vita-controls.md).

## Building

Tested on x86-64 Linux. Every target needs Python 3, CMake 3.16+ and Ninja.

- PS Vita: [VitaSDK](https://vitasdk.org) (`VITASDK`) with the vitaGL, vitaShaRK, SceShaccCgExt and math-neon packages (`vdpm`).
- Linux: clang.

### 1. Translate

Copy the ROM files listed above to `rom/`, then:

```sh
scripts/translate.sh
```

This writes `gen/m68k/`. `rom/` and `gen/` are not part of the repository.

### 2. Build

PS Vita (`build/vita/mystwarr.vpk`):

```sh
scripts/build-vita.sh
```

LiveArea images are optional and not part of the repository: `vita/sce_sys/icon0.png` (128x128),
`vita/sce_sys/livearea/contents/bg.png` (840x500) and `startup.png` (280x158) are packed when present; the PNGs must be 8-bit indexed.

Linux (`build/desktop/mw_desktop`):

```sh
scripts/build-desktop.sh
MW_ROOT=$PWD MW_PNG=/some/folder MW_PNG_EVERY=300 build/desktop/mw_desktop 3000     # 3000 frames, a PNG every 300
```

## Credits

Licences of the linked libraries and the tools used: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

- [VitaSDK](https://vitasdk.org)
- [vitaGL](https://github.com/Rinnegatamante/vitaGL), [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) — Rinnegatamante (LGPL-3.0)
- [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) — bythos14 (LGPL-2.1)
- [math-neon](https://github.com/Rinnegatamante/math-neon) — Lachlan Tychsen-Smith (MIT)
- [MAME](https://github.com/mamedev/mame) (BSD-3-Clause drivers): hardware reference for the GX128 board; its video and sound behaviour follows MAME's drivers (see the copyright holders in [LICENSE-MAME.txt](LICENSE-MAME.txt)). MAME was also the reference for checking pictures.
- [psp2-tooling](https://github.com/Grarak/psp2-tooling) — Grarak (GPL-3.0): testing and measuring on a PS Vita
- [Vita3K](https://github.com/Vita3K/Vita3K) (GPL-2.0): desktop tests of the PS Vita build

## License

The code in this repository is licensed under the GNU General Public License v3.0 (`LICENSE`), copyright Tobic-99, except the behaviour taken from MAME's drivers (BSD-3-Clause, `LICENSE-MAME.txt`). Third-party components keep their own licences. Mystic Warriors, its name and its artwork are the property of
their rights holders; no ROM data is included.
