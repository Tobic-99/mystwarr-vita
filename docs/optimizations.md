# How the port runs on the PS Vita

The board runs at 59.19 Hz (264 lines of 64 µs). Measured on a PS Vita at 444 MHz in a quiet part of stage 1: mostly 59.2 fps, the translated 68000
code about 2 ms per frame, drawing about 5 ms (split over two cores), the sound about 45% of a core.

## Threads

| Thread | Core | Work |
|---|---|---|
| emu | 0 | translated 68000 code on its own stack, board, one scanline slice at a time, paced to the board's 59.19 Hz |
| render | 1 | draws part of the frame and presents it |
| band | 0 | draws the rest of the frame |
| statewriter | 1 | writes save states in the background |
| audio | 2 | the sound board, paced by the audio output |

## CPU

`tools/m68k_discover.py` finds the code (vectors, branches, tables, plus addresses recorded at run time in `tools/m68k_learned.txt`);
`tools/m68krec.py` writes one C++ function per guest function, with 4 KB page tables for memory. The 68000 runs on its own stack and
yields at backward branches when its slice of cycles is used.

## Save states

That stack sits in the middle of a call chain and cannot be copied, because the game loads at a different address on each launch. So a
state is taken and restored only at the game's idle wait, where the call chain is always the same; on load the game boots to that point
and the machine state is swapped in.

## Video

The finished frame is copied straight into the texture's memory: `glTexSubImage2D` takes about 8 ms a frame, the copy about 1 ms. Two
textures keep the GPU from reading the one being written.
