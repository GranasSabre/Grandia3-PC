# Grandia III for PC

**Grandia III** (PlayStation 2, 2005) as a real PC game: sharper, wider and smoother than on the
console, with savestates, subtitles and a German and French translation - and, if you like, with the
original Japanese voices.

This is not an emulator. The game's program was translated into PC code once, ahead of time, so the game
runs on your PC like any other PC game.

**No game data is included.** You need your own copy of *Grandia III (USA)*. The program reads the disc
images (ISO files) of your discs while you play; it never changes or copies them.

This is a fan project, not affiliated with Square Enix or Game Arts.

## Download and play

1. Download `Grandia3-PC-v*.zip` from the [Releases](../../releases) page and unzip it into a folder of
   its own.
2. Start `Grandia3.exe` and choose the image of your disc 1 (for example
   `Grandia III (USA) (Disc 1).iso`).

Tip: put the image next to `Grandia3.exe` as `disc1.iso` (disc 2: `disc2.iso`) and the game finds it
by itself - no file dialog, handy on the Steam Deck.

**Requirements:** Windows 10 or 11 (64 bit) with a graphics card for Direct3D 11.3, or a Steam Deck /
Linux with Proton.

Updating: unzip the new version over the old folder. Settings, saves and savestates stay.

Everything about keys, settings and options: [docs/README.txt](docs/README.txt)
(German: [docs/LIESMICH.txt](docs/LIESMICH.txt)).

## What you get

**Picture**
- Up to 8 times the original resolution, chosen automatically for your screen
- Real widescreen (16:9, 16:10, or ultrawide 21:9 and 32:9), with menus and text kept in shape
- 60 fps in the field and, if you want, in battles - at the correct game speed; cutscenes run at
  30 fps like the original, so they stay in sync with the voices
- Sharp text, sharpening and image filters
- Optional HD texture pack (see below)

**Comfort**
- Savestates anywhere, also in battles and cutscenes: 9 slots, plus an automatic one after every map
  change
- Fast forward (hold Tab, or Back + R2 on the controller)
- Skip cutscenes and movies with START
- Turn off battles on monster contact to explore in peace (story and boss battles still happen)
- Large dialogue windows for small screens (automatic on the Steam Deck)
- An in-game settings menu (F1, or Back + Start / the PS button), its size adjustable

**Sound and language**
- The original English text, or complete German and French fan translations
- Subtitles for all voiced cutscenes and movies, in English, German or French (these scenes have no
  text in the original)
- **Japanese voices:** with an image of your own *Grandia III (Japan)* disc 1, cutscenes, battle shouts
  and movies play with the original Japanese voices, while text and subtitles stay in your language
- Separate volume for music, voices, effects and movies

**Controls**
- Xbox and other XInput controllers, and the DualShock 4 and DualSense (USB or Bluetooth) directly,
  with rumble - Steam Input and DS4Windows keep working too
- Keyboard keys can be changed in the settings

**Steam Deck**
- Recognised automatically: 16:10 widescreen, full screen, large dialogue windows and larger menus
- Shortcuts on the controller: View + Menu = settings, View + R1 / L1 = save / load a savestate,
  View + R2 = fast forward
- Add `Grandia3.exe` to Steam as a non-Steam game and choose Proton in its compatibility settings

## Japanese voices

Put the image of your Japanese disc 1 next to the game or next to your US image, named
`disc1_jp.iso` or `Grandia III (Japan) (Disc 1).iso`. Then switch on *Japanese voices* in the F1 menu
and restart the game. If the image is not found, the game asks for it once. The Japanese disc covers
disc 2 as well; only the disc 2 movies keep their English sound.

## HD texture pack (optional)

An unofficial pack with all 7,604 textures of the game upscaled 4 times (7.5 GB, separate download):
[part 1](https://mega.nz/file/HsAA1ZJI#XaEf7ifCbabg8oY0FB5-TBd1A7-65ooXDTEMhyZIy5U) and
[part 2](https://mega.nz/file/C9JmECLB#aYbkB_SHfTTdox-uphb7gvy9ABdE9py6nxQxjE7xhjk). Unzip both into the
folder of `Grandia3.exe` so that a `textures` folder sits next to it; F8 switches the textures on and
off. The pack only works with this version, not with PCSX2.

## Problems?

The game writes `grandia3.log` next to itself. Please attach it when you report a problem in the
[Issues](../../issues). Feedback on the subtitles (timing, wrong or missing lines) and the translations
is welcome too.

## How it works (short)

A PS2 game is a program for the PS2's processors. Here that program - together with the game's sound
driver and its 3D geometry programs - was translated into C++ code by tools in this repository, and that
code is compiled into a normal Windows program. A small runtime takes the place of the PS2 hardware the
game talks to: it draws the pictures with Direct3D 11, plays the sound, and reads the discs. Because the
code is generated from your own disc, this repository contains no game code at all.

## Building from source

For developers; players only need the download above.

1. Toolchain: [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw/releases) (`llvm-mingw-*-ucrt-x86_64`)
   and [Ninja](https://github.com/ninja-build/ninja/releases), unpacked into `toolchain/` (paths at the
   top of `build.py`), and Python 3.
2. Extract disc 1: `python tools/iso_extract.py "Grandia III (USA) (Disc 1).iso" extract extracted/disc1`
3. Generate the game code: `python tools/gen_all.py extracted/disc1 generated` and
   `python tools/iop_recomp.py extracted/disc1/GS_IOP.IRX 0x00040000 generated/iop gsiop`
4. Build: `python build.py` (result: `build/release/Grandia3.exe`)
5. The 3D geometry programs: a build without their translation runs them in an interpreter and saves
   them into `vu_new/`; `python tools/vu_recomp.py vu_new/vu1img_<hash>.bin generated/vu/vu1_<hash>.cpp`
   and a rebuild add the translation.

Switches for testing and development are listed in `runtime/src/main.cpp`.

## License

The code in this repository (the translation tools and the runtime) is under the MIT license - see
[LICENSE](LICENSE). It covers only what was written for this project: *Grandia III* and everything in it
(game code, text, audio, images, trademarks) belongs to Square Enix and Game Arts; nothing here grants
rights to it, and the language and subtitle files are fan translations of game content for private use.

## Credits

- Widescreen patch values from the PCSX2 patch database (SLUS-21334, by nemesis2000)
- Built with LLVM-MinGW and Ninja
