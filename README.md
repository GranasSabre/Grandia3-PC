# Grandia III for PC

A native Windows version of **Grandia III** (PlayStation 2, 2005), made by static recompilation: the
game's MIPS code, its sound driver and its VU1 geometry programs are translated ahead of time into C/C++
and compiled for x86-64. A small runtime stands in for the PS2 hardware the code talks to (graphics via
Direct3D 11, sound, DMA, memory cards). Nothing is interpreted at run time.

**No game data is included.** You need your own copy of *Grandia III (USA)*: the program reads the disc
images (ISO) of disc 1 and disc 2 at run time and does not change or copy them.

This is a fan project, not affiliated with Square Enix or Game Arts.

## Download and play

Get `Grandia3-PC-v*.zip` from the [Releases](../../releases), unzip it, start `Grandia3.exe` and choose
your disc 1 image when asked - or put the image next to the exe as `disc1.iso` (disc 2: `disc2.iso`) and it
is found without a file dialog (handy on the Steam Deck). Details, keys and settings: [docs/README.txt](docs/README.txt)
(German: [docs/LIESMICH.txt](docs/LIESMICH.txt)).

Works on Windows 10/11 (Direct3D 11.3 graphics card) and on the Steam Deck / Linux with Proton.

**Optional HD texture pack (4x, unofficial, separate download, 7.5 GB):**
[part 1](https://mega.nz/file/HsAA1ZJI#XaEf7ifCbabg8oY0FB5-TBd1A7-65ooXDTEMhyZIy5U) and
[part 2](https://mega.nz/file/C9JmECLB#aYbkB_SHfTTdox-uphb7gvy9ABdE9py6nxQxjE7xhjk) - unzip both into the folder
of `Grandia3.exe` so that a `textures` folder sits next to it; F8 switches the textures on and off. It only
works with this port, not with PCSX2.

## Features

- Internal resolution up to 8x, automatic choice for your screen
- Widescreen: a real 16:9 or 16:10 view of the 3D scenes, menus and text keep their 4:3 proportions
- 60 fps in the field and (optionally) in battles at correct game speed, automatic fallback to 30
- Savestates everywhere (9 slots), skippable cutscenes and movies
- Large dialogue windows for small screens (Steam Deck: automatic), sharp text, sharpening, filters
- Steam Deck: detected automatically (16:10 view, full screen, large windows, controller shortcuts)
- Languages: original English text or a complete German translation; more via `lang/<code>.tsv`
- Subtitles for voiced cutscenes and movies (experimental, English, so far up to Alonso's ship): the
  runtime follows the voice stream on its way to the sound chip (`runtime/src/platform/subtitles.cpp`),
  the timings come from the voice-over script (`tools/subs`)
- HD texture packs: PNG files in a `textures` folder replace the game's textures

## How it works

| Part | How it is handled |
|---|---|
| Game code (EE, MIPS R5900) | `tools/recomp.py` + `tools/gen_all.py`: one C function per game function, overlays (field, battle, flight) included |
| Sound driver (IOP module GS_IOP.IRX) | `tools/iop_recomp.py`, runs on a host thread like on the IOP |
| VU1 geometry programs | `tools/vu_recomp.py`: ahead-of-time translation of the microcode image, verified against an interpreter (`--vu-verify`) |
| PS2 kernel, IOP services | high-level replacements (`runtime/src/ee/kernel.cpp`, `runtime/src/iop`) |
| Graphics Synthesizer | Direct3D 11.3 renderer emulating GS blending with rasterizer ordered views (`runtime/src/gs`) |
| VIF1/VU1/GS front end | own thread (`runtime/src/hw/gspipe.cpp`) |
| Savestates | build-independent: threads resume at MIPS level (`runtime/src/savestate.cpp`) |

## Building from source

The repository contains no generated code: it is produced from your own disc.

1. Toolchain: [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw/releases) (`llvm-mingw-*-ucrt-x86_64`)
   and [Ninja](https://github.com/ninja-build/ninja/releases), unpacked into `toolchain/`
   (the paths are set at the top of `build.py`). Python 3 for the tools.
2. Extract disc 1 (disc 2 carries the same program files):
   `python tools/iso_extract.py "Grandia III (USA) (Disc 1).iso" extract extracted/disc1`
3. Generate the game code:
   `python tools/gen_all.py extracted/disc1 generated` and
   `python tools/iop_recomp.py extracted/disc1/GS_IOP.IRX 0x00040000 generated/iop gsiop`
4. Build: `python build.py` (output: `build/release/Grandia3.exe`).
5. VU1: a build without a VU1 translation runs the microcode in the interpreter and saves the image
   into `vu_new/`; `python tools/vu_recomp.py vu_new/vu1img_<hash>.bin generated/vu/vu1_<hash>.cpp`
   and a rebuild add the translation.

Useful switches for development are listed in `runtime/src/main.cpp` (`--perf`, `--vu-verify`,
`--resume-test`, `--log=...`, `--press=...` for scripted input, ...).

## Credits

- Widescreen patch values from the PCSX2 patch database (SLUS-21334, by nemesis2000)
- Built with LLVM-MinGW and Ninja
