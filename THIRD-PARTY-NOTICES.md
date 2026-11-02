# Third-party components

Linked into `eboot.bin` or packed in the VPK:

| Component | Licence | Source |
|---|---|---|
| vitaGL (Rinnegatamante) | LGPL-3.0 | https://github.com/Rinnegatamante/vitaGL |
| vitaShaRK (Rinnegatamante) | LGPL-3.0 | https://github.com/Rinnegatamante/vitaShaRK |
| SceShaccCgExt (bythos14) | LGPL-2.1 | https://github.com/bythos14/SceShaccCgExt |
| math-neon (Lachlan Tychsen-Smith) | MIT | https://github.com/Rinnegatamante/math-neon |
| MAME hardware behaviour | BSD-3-Clause, `LICENSE-MAME.txt` | https://github.com/mamedev/mame |
| VitaSDK stubs (vita-headers, vita-toolchain) | MIT | https://github.com/vitasdk |
| newlib (C library) | BSD-style licences (COPYING.NEWLIB) | https://github.com/vitasdk/newlib |
| libstdc++, libgcc | GPL-3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org |

vitaGL, vitaShaRK and SceShaccCgExt are linked statically. The complete source of this port and its build scripts are in this
repository, so the PS Vita build can be rebuilt and relinked with modified versions of those libraries (`scripts/build-vita.sh`).

Not distributed: `libshacccg.suprx` (Sony), which vitaGL loads from the PS Vita to compile its shaders.

# Used to develop and test the port (not linked or distributed)

| Tool | Licence | Source |
|---|---|---|
| psp2-tooling (Grarak): tests and measurements on a PS Vita | GPL-3.0 | https://github.com/Grarak/psp2-tooling |
| MAME: picture and sound reference | GPL-2.0-or-later as a whole | https://github.com/mamedev/mame |
| Vita3K: desktop tests of the PS Vita build | GPL-2.0 | https://github.com/Vita3K/Vita3K |
