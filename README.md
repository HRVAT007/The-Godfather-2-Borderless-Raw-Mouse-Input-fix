# The Godfather II — Borderless & Raw Mouse Input Fix

An unofficial ASI plugin (`gf2fix.asi`) for **The Godfather II** (2009, PC) that adds two
quality-of-life fixes the game never shipped with:

- **Borderless windowed** rendering at your desktop resolution and refresh rate (no more
  exclusive-fullscreen mode switching, fast alt-tab).
- **True 1:1 raw mouse input** — no acceleration, smoothing, or deadzone — delivered through
  the game's native DirectInput path.

Loaded at runtime by an **Ultimate ASI Loader** (`dinput8.dll` proxy); the game files are never
modified.

---

## Requirements

- A legally owned, installed copy of **The Godfather II** (PC).
- **Ultimate ASI Loader (32-bit)** — provides the `dinput8.dll` proxy that loads `.asi` plugins.
  <https://github.com/ThirteenAG/Ultimate-ASI-Loader>
- Windows. The plugin is a **32-bit** DLL and must match the game's 32-bit process.

> **Do not run Fullscreenizer (or similar borderless wrappers) alongside this plugin.** A
> `fullscreenizer.cfg` in the game folder conflicts with the plugin's own Direct3D borderless
> hook and will break the display mode.

---

## Installation

1. Install **Ultimate ASI Loader** into the game directory (copy its `dinput8.dll` next to
   `godfather2.exe`).
2. Download the latest **`gf2fix.asi`** from the [Releases](../../releases) page.
3. Copy `gf2fix.asi` into the game directory, next to `godfather2.exe`:

   ```
   ...\The Godfather II\
       godfather2.exe
       dinput8.dll        <- Ultimate ASI Loader
       gf2fix.asi         <- this plugin
       gf2fix.ini         <- auto-generated on first run (editable)
   ```

4. Launch the game. The fixes are active immediately, and a documented **`gf2fix.ini` is created
   automatically** on first run so you can tweak settings. (If the game folder is read-only, the
   plugin still works using its built-in defaults.)

---

## Configuration (`gf2fix.ini`)

`gf2fix.ini` is **auto-generated next to `godfather2.exe` on first run**. All keys are optional;
the defaults below work for most setups. Edit the file and relaunch to apply changes.

```ini
[Borderless]
Enabled=1            ; 1 = borderless windowed at desktop res/refresh, 0 = leave game default

[RawInput]
Enabled=1            ; 1 = 1:1 raw mouse, 0 = game default mouse
Sensitivity=1.0      ; mouse multiplier (1.0 = true 1:1; raise/lower to taste)

[UI]
TextScale=1.0        ; HUD text glyph multiplier (1.0 = off). Does NOT affect subtitles.
SubtitleHook=0       ; experimental D3D9 draw-call hooks. LEAVE AT 0 (see Limitations).

[Debug]
Log=0                ; write gf2fix.log next to the exe
LogApt=0             ; verbose APT UI diagnostics
LogFiles=0           ; trace every non-system file the engine opens (boot diagnostics)
```

---

## Building from source

1. Install **Visual Studio 2022** with the *Desktop development with C++* workload.
2. Clone this repository (MinHook is vendored under `minhook/`).
3. Run `build.bat`. It auto-detects Visual Studio via `vswhere`, compiles a 32-bit DLL, and
   outputs `gf2fix.asi`.

```
build.bat
```

The build links MinHook's sources directly (`minhook/src/*.c`); no separate library step is
needed.

---

## How it works

- **Borderless:** hooks `Direct3DCreate9` → `IDirect3D9::CreateDevice`, rewrites the present
  parameters to windowed at the desktop resolution/refresh, then restyles the window to a
  borderless topmost-free fullscreen rect. `IDirect3DDevice9::Reset` (vtable index 16) is hooked
  to re-apply on mode changes.
- **Raw mouse:** hooks `DirectInput8Create` and the mouse device's `GetDeviceState` /
  `GetDeviceData` / `SetDataFormat` / `Acquire` to pass through unfiltered relative motion,
  plus `GetCursorPos` / `SetCursorPos` / `ClipCursor` for a consistent virtual cursor.
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
  playback (the same risk class as above). The in-game `movies/common/logo.vp6` clip can be
  removed by renaming it, which the engine tolerates, but that does not affect the Flash logos.

---

## Reverse-engineering tools

`tools/` contains the Python/C++ helpers used to investigate the (SecuROM-packed) executable:

- `re_helper.py` — disassemble at a file offset / search strings in the PE.
- `scan_tables.py`, `scan_strings.py` — locate APT function-pointer tables and string references.
- `analyze_capture.py` — parse runtime APT capture logs.
- `vidx_test.cpp` / `vidx_build.bat` — offline proof that the MSVC pointer-to-member vtable-index
  trick returns garbage on this toolchain (the root cause of the early boot crashes).

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
