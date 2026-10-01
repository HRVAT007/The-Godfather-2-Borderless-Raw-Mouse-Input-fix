# The Godfather II — Borderless, Raw Mouse & Camera Fix

An unofficial ASI plugin for **The Godfather II** (2009, PC) that adds the fixes the game never
shipped with:

- **True 1:1 raw mouse input** — no acceleration, smoothing, or deadzone — delivered through
  the game's native DirectInput path.
- **The mouse actually drives the camera in a car.** The game divides its vehicle look by a
  hardcoded console frame rate and then integrates it through a slow spring, so on a modern PC
  the camera turns far less than your mouse does, slides lazily behind it, and gives up vertical
  control entirely as soon as the car moves. This fix corrects all of it, and the cover and
  aim-down-sights cameras, which are integrated the same way, come along with it.
- **Borderless windowed** rendering at your desktop resolution and refresh rate (no more
  exclusive-fullscreen mode switching, fast alt-tab) — **off by default**, because Modern Fixes
  already provides it and its supersampling depends on its own implementation. Enable it for a
  plain ASI loader, see [Configuration](#configuration-gf2fixini).

Loaded at runtime by an ASI loader (`dinput8.dll` proxy); the plugin patches nothing on disk.

---

## Requirements

- A legally owned, installed copy of **The Godfather II** (PC).
- An **ASI loader**. Either is fine, and **The Godfather II – Modern Fixes** is recommended because
  it also brings widescreen/UI fixes, FOV, and a modern-CPU startup fix, and its own `dinput8.dll`
  proxy loads this plugin:
  - **Modern Fixes** — its `dinput8.dll` loads every `.asi` in `<game>\scripts\`.
  - **Ultimate ASI Loader (32-bit)** — <https://github.com/ThirteenAG/Ultimate-ASI-Loader>,
    loads `.asi` files from the game directory.
- Windows. The plugin is a **32-bit** DLL and must match the game's 32-bit process.

> **Name the file `rawmouse.asi`, not `gf2fix.asi`.** Modern Fixes treats any ASI whose name
> contains `GF2` as a competing mod and *yields its shared executable patches to it* — including
> the worker-thread startup fix — which makes the game crash on boot on modern CPUs. Renaming it
> back to `rawmouse.asi` is all it takes; the plugin itself is unaffected by its own filename.

> **Do not run Fullscreenizer (or similar borderless wrappers) alongside this plugin.** A
> `fullscreenizer.cfg` in the game folder conflicts with the plugin's own Direct3D borderless
> hook and will break the display mode.

---

## Installation

With **Modern Fixes** already installed (the common case):

1. Download the latest **`rawmouse.asi`** from the [Releases](../../releases) page.
2. Copy it into `<game>\scripts\`, next to `ModernFixes.asi`:

   ```
   ...\The Godfather II\
       godfather2.exe
       dinput8.dll                <- the ASI loader
       scripts\
           ModernFixes.asi
           ModernFixes.ini
           rawmouse.asi           <- this plugin
           gf2fix.ini             <- auto-generated on first run (editable)
           gf2fix.log             <- only while [Debug] Log=1
   ```

3. Launch the game. The mouse and camera fixes are active immediately, and **`gf2fix.ini` is
   created next to the plugin on first run** with the recommended values — you do not need to
   open it. Leave `[Borderless] Enabled=0` here: Modern Fixes is already running the game
   borderless at your desktop resolution and refresh rate.

With a plain **Ultimate ASI Loader**: copy the same file into the game directory (still named
`rawmouse.asi`); the loader looks for `.ini`/`.log` next to the plugin there, so nothing else
changes — but set `[Borderless] Enabled=1`, since that loader has no borderless option of its
own.

---

## Configuration (`gf2fix.ini`)

The generated file already holds the recommended values, so editing is optional. **Every `[Camera]`
key is re-read every 2 seconds while playing**, so the camera feel can be retuned mid-drive without
restarting. Delete the file to get these values back.

```ini
[Borderless]
Enabled=0            ; 1 = borderless windowed at desktop res/refresh. Leave 0 with Modern
                     ; Fixes installed - it already does this, and its supersampling requires
                     ; its own borderless mode. Use 1 with a plain ASI loader.

[RawInput]
Enabled=1            ; 1 = 1:1 raw mouse, 0 = game default mouse
Sensitivity=1.0      ; mouse multiplier (1.0 = true 1:1; raise it only if 1.0 feels slow)

[Camera]
CarLookScale=1.0     ; how much of your mouse movement the camera keeps at any fps
                     ;   0 = leave the engine alone, 2.0 = what walking on foot effectively uses
CarPitchMouse=1      ; 1 = the mouse keeps vertical control while the car is moving
CarSpringK=5000      ; how fast the view catches up with the mouse (engine value is 175)
CarCentreIdle=2.5    ; seconds of no mouse movement before the view eases back behind the car
CarHoldK=5           ; stiffness during that wait - low keeps the view where you left it
CarCentreK=40        ; stiffness for the glide back - a glide lasts roughly 100/k seconds
CarCam60=0.40        ; how strongly the camera position is pulled onto the car, 0..1.
                     ;   1.00 removes that smoothing entirely and looks jittery over bumps.
CarCentreDelay=1000000 ; seconds of still mouse before the chase cam latches onto the car's
CarSpeedThresh=1000000 ; vehicle speed at which the game starts damping both look axes
                       ; (engine values 0.1 and 25.0 - both switched far out of reach here)

[UI]
TextScale=1.0        ; HUD text glyph multiplier (1.0 = off). Does NOT affect subtitles.

[Debug]
Log=0                ; 1 = write gf2fix.log next to this file
```

If the folder holding the plugin is read-only, the ini cannot be written and the plugin runs on
its built-in defaults, which are the values above.

**Advanced keys** exist in the source but are left out of the generated file on purpose, because
they are either inert on the code path the game actually uses or unsafe to enable:
`RawInput\CursorHooks` (double-detours user32 alongside other mods — crashes boot),
`UI\SubtitleHook` (see Limitations), `Camera\FilterBypass`, `FilterGain`, `LookGain`,
`RefYawTarget`, `RefPitchTarget`, `DeDamp`, `Smooth`, `RampTime`, `CarNoSmooth`, and
`Debug\CameraProbe`, `LogApt`, `LogFiles`, `AptHooks`, `FilterLog`. Adding them to the ini is
supported; their behaviour is documented in `src/gf2fix.cpp`.

---

## Building from source

1. Install **Visual Studio 2022** with the *Desktop development with C++* workload.
2. Clone this repository (MinHook is vendored under `minhook/`).
3. Run `build.bat`. It auto-detects Visual Studio via `vswhere`, compiles a 32-bit DLL from
   `src\gf2fix.cpp`, and outputs **`rawmouse.asi`** — the install name, so a local build cannot be
   dropped in under the wrong name by accident.

```
build.bat
```

The build links MinHook's sources directly (`minhook/src/*.c`); no separate library step is needed.

---

## How it works

- **Borderless:** hooks `Direct3DCreate9` → `IDirect3D9::CreateDevice`, rewrites the present
  parameters to windowed at the desktop resolution/refresh, then restyles the window to a
  borderless topmost-free fullscreen rect. `IDirect3DDevice9::Reset` (vtable index 16) is hooked
  to re-apply on mode changes.
- **Raw mouse:** hooks `DirectInput8Create` and the mouse device's `GetDeviceState` /
  `GetDeviceData` / `SetDataFormat` / `Acquire` to pass through unfiltered relative motion,
  plus `GetCursorPos` / `SetCursorPos` / `ClipCursor` for a consistent virtual cursor.
- **Camera:** the vehicle look paths divide by a constant the engine initialises to `1/59.94`, a
  console frame rate, so the faster your GPU runs the less of each mouse movement survives; the
  fix re-keys that divisor to the real frametime. Pitch is separately gated off above a 0.001
  speed epsilon by a branch that hands the axis to an automatic system; the fix makes that branch
  unconditional. Finally the look spring's stiffness is raised (its catch-up rate is
  `stiffness · frametime²`, so an engine value tuned for 30 fps consoles is about five times too
  slow at 150 fps), and that stiffness is switched by phase — stiff while you steer, soft while
  the mouse is idle — so the view stays where you left it and then eases back instead of snapping.
  The stiffness is additionally capped from the measured frametime, so a hitch costs response for
  a second rather than making the spring overshoot.
- Hooks are installed with **MinHook** (inline) and direct COM vtable patching.

See `src/gf2fix.cpp` for the full implementation.

---

## Known limitations

- **Subtitles do not scale.** Subtitle glyphs are drawn by a movie-UI subsystem that lives
  outside every hookable APT text path (verified at runtime: zero calls through the APT
  `DrawString` family during cutscenes). The only viable route — a D3D9 draw-call vertex scaler —
  requires COM vtable indices that cannot be resolved safely on this toolchain and caused boot
  crashes during development. `SubtitleHook` is therefore **off by default**; enabling it is
  unsupported and may prevent the game from booting.
- **Publisher/studio boot logos are not skipped.** A runtime file trace proved the logos are a
  Flash/APT animation packed inside the compressed `frontend.str` archive — not a removable movie
  file, exe resource, or config flag. Removing them would require hooking packed frontend
  playback (the same risk class as above).
- **Frame-rate-dependent feel.** The camera values above were tuned at ~150 fps; they are correct
  in principle at any frame rate (the divisor and the spring cap both track the real frametime),
  but `CarSpringK`/`CarCentreK` describe a rate, so drop them if the view starts to feel harsh on
  a much lower frame rate.

---

## Troubleshooting

1. Set `[Debug] Log=1` in `gf2fix.ini` and relaunch: `gf2fix.log` appears next to the plugin and
   records which hooks installed, which camera paths were scaled, and any lever that refused to
   apply because the executable did not match what it expected. Every such lever disables itself
   and says so rather than crashing.
2. If the game does not boot at all, check the file is not named `gf2*` (see the warning above),
   and read `<game>\scripts\ModernFixes.log` — with `[Debug] Log=1` there, Modern Fixes prints its
   full exe-patch table and marks anything it yielded to another mod.
3. Delete `gf2fix.ini` to rule out a bad value; the plugin regenerates the recommended one.

---

## Reverse-engineering tools

`tools/` contains the helpers used to investigate the executable:

- `re_helper.py` — PE section/address mapping, disassembly at a file offset, literal data reads
  (needs `pefile` and `capstone`, and an unpacked `godfather2.exe`).
- `imap.py`, `qscan.py` — find every reference to a given address or constant in the image.
- `vidx_c.c` — offline, provably correct COM vtable index resolution (`CINTERFACE` + `offsetof`);
  kept because the tempting MSVC pointer-to-member shortcut silently returns byte offsets instead
  of indices, which is what caused the early boot crashes.

These are investigation aids, not part of the shipped plugin.

---

## Credits

- **[MinHook](https://github.com/TsudaKageyu/minhook)** — minimal x86/x64 hooking library
  (BSD 2-Clause). Vendored under `minhook/`; see `minhook/LICENSE.txt`.

## License

MIT © 2026 HRVAT007 — see [LICENSE](LICENSE).

## Disclaimer

This is an unofficial, fan-made modification. It is not affiliated with, endorsed by, or
associated with Electronic Arts, Paramount Pictures, or the rights holders of The Godfather II.
It requires a legally owned copy of the game and patches nothing on disk — all changes are
in-memory at runtime.
