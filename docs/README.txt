Grandia III for PC (Grandia3-PC)
================================

A native Windows version of Grandia III (PlayStation 2), made by static recompilation: the game's
program was translated ahead of time into PC code. Nothing is interpreted at run time; the game runs
like a PC game, with higher resolutions, widescreen, 60 fps, savestates and more.

No game data is included. You need your own copy of Grandia III (USA) - disc images (ISO) of disc 1
and, later in the game, disc 2. The program reads them as they are; nothing is copied or changed.

This is a fan project. It is not affiliated with Square Enix or Game Arts.


Requirements
------------
- Windows 10/11 (64 bit) with a Direct3D 11.3 capable graphics card, or a Steam Deck / Linux with Proton
- Grandia III (USA) disc images (Redump dumps: "Grandia III (USA) (Disc 1).iso" and "(Disc 2).iso")
- About 40 MB for the program; the ISOs stay where they are


Installation
------------
1. Unzip the archive into a folder of its own.
2. Start Grandia3.exe.
3. At the first start it asks for the image of your disc 1. Choose the .iso file. The path is stored in
   grandia3.ini; later starts go straight to the game.

Tip: put the ISO into the program folder, named "disc1.iso" (disc 2: "disc2.iso") or with its Redump
name "Grandia III (USA) (Disc 1).iso". It is then found on its own and no file dialog appears.

Changing to disc 2 works as on the console: save at the end of disc 1, then choose "Change disc" in the
F1 menu. The game restarts with disc 2 (it asks for that image once); load your save there.

Saves go to the folder "memcards" (PS2 memory card files), savestates to "savestates".


Steam Deck
----------
- Copy the folder to the Deck, add Grandia3.exe in Steam as a "non-Steam game" and set its
  compatibility tool to Proton (Properties > Compatibility).
- Easiest: copy your ISOs into the same folder as "disc1.iso" and "disc2.iso" (or keep their Redump
  names). Then the game finds them by itself - no file dialog, which is handy in Game Mode.
- The Deck is detected automatically: on the first start the game uses a real 16:10 widescreen view,
  full screen and large dialogue windows.
- Controls: View + Menu = settings menu, View + R1 = save a savestate, View + L1 = load it.
  The settings menu and the savestate notes are drawn larger on the Deck; the menu scrolls.
- Tested on a Steam Deck (Proton Experimental): the field runs at 60 fps with the 60 fps option,
  cutscenes at 30 like the original. If a scene cannot reach 60 fps, the game drops back to its native
  30 fps there by itself. If an effect-heavy scene still runs below 30, set "Internal resolution" in the
  F1 menu to 1x. Reports are welcome.
- If turning the camera with the right stick feels the wrong way round: F1 menu, Controls,
  "Invert camera".


Keys
----
F1 / Back+Start   settings menu (the game pauses; change values with left/right)
F3                60 fps on/off
F4                sharpening on/off
F5 / Back+R1      save a savestate (slot chosen in the F1 menu)
F9 / Back+L1      load a savestate
F6                widescreen on/off
F7                show the frame rate (top left of the picture)
F8                HD textures on/off (if a texture pack is installed)
F11, Alt+Enter    full screen
Tab (hold)        fast forward

Keyboard (default, changeable in the settings): arrow keys = d-pad, Shift + arrows = run,
S = Cross, D = Circle, A = Square, W = Triangle, Q/E = L1/R1, 1/3 = L2/R2, Enter = Start,
Backspace = Select. Controllers (XInput) work out of the box, including rumble.


Features
--------
- Internal resolution up to 8x (automatic: matching your screen height)
- Widescreen: a real 16:9 or 16:10 view of the 3D scenes, or ultrawide (21:9, 32:9: aspect "Screen");
  menus and text keep their 4:3 proportions
- 60 fps in the field and (optionally) in battles, with correct game speed; a scene that cannot hold
  60 fps falls back to 30 by itself; in-engine cutscenes run at 30 like the original (in sync with
  their voices)
- Savestates (9 slots) everywhere, also in battles and cutscenes
- Skip cutscenes and movies with START
- Large dialogue windows (1.25x, text included) for small screens - automatic on the Steam Deck
  (needs the widescreen view)
- Sharp text at high resolutions, contrast adaptive sharpening, image filters
- Separate volume for music, voices, effects and movies
- Subtitles for voiced cutscenes and movies without a text box (experimental, English): so far from
  the opening up to the night on Alonso's ship after the casino with Bianca; later scenes follow.
  Switch: F1 menu "Subtitles in cutscenes". Please report wrong timing or wrong/missing lines.
- Language: the original English text, German or French (complete fan translations, French is new
  and has seen little play testing so far). More languages can be added as lang\<code>.tsv files.
- HD texture packs: images in a folder "textures" next to the program replace the game's textures
  (not part of this archive). An unofficial 4x pack (7.5 GB, two parts - unzip both next to the exe):
  https://mega.nz/file/HsAA1ZJI#XaEf7ifCbabg8oY0FB5-TBd1A7-65ooXDTEMhyZIy5U
  https://mega.nz/file/C9JmECLB#aYbkB_SHfTTdox-uphb7gvy9ABdE9py6nxQxjE7xhjk


Settings (grandia3.ini)
-----------------------
The F1 menu covers everything; grandia3.ini holds the same settings, a few more, and comments.
Some of them:
  language = en          en = original English text, de = German, fr = French
  internal_scale = 0     0 = automatic, 1 = original 512 x 448, 2 .. 8
  aspect = 0             0 = 4:3, 1 = 16:9, 2 = fill the window, 3 = 16:10, 4 = screen (ultrawide)
  widescreen_hack = 1    widescreen view
  big_text = -1          large dialogue windows: -1 automatic (Steam Deck), 0 off, 1 on
  text_scale = 1.25      their size
  fps60 = 0              60 fps (F3)
  gs_thread = 1          3D geometry on a processor core of its own (faster)
  subtitles = 1          subtitles in cutscenes (lang\sub_en.tsv)
  camera_invert_x = 0    1 = right stick left/right reversed (camera turns the other way)


Problems?
---------
The program writes grandia3.log next to itself. Please attach it to a bug report.
If the picture stays black or the game is very slow, the graphics driver may not support the needed
Direct3D 11.3 features; renderer = software in grandia3.ini is a (slow) fallback.


License
-------
The program code is under the MIT license (source code and LICENSE:
https://github.com/GranasSabre/Grandia3-PC). Grandia III and everything in it belong to Square Enix
and Game Arts.
