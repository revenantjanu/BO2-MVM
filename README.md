# BO2 Zombies MVM

A cinematic / movie-maker tool for **Black Ops 2 Zombies on Plutonium (T6), LAN / offline only**.
Built for making cinematics and trickshot edits: free camera, camera paths, slow motion, spawnable
zombies to pose, effects, and video capture.

> **Status: work in progress.** This is not finished or fully tested. Expect bugs and missing pieces.
> **The full source is included** (C++ DLL, C# launcher, GSC script) so you can fix it, change it and
> build on it however you like. See [License](#license).

## What it does
- **Free camera** with roll and FOV, fast/slow modifiers
- **Camera paths** with keyframes: per-key ease / speed / handles, spline or linear, undo/redo, scrubber, speed ramps
- **One-key path recording** (F9) with countdown, smoothing and simplification, plus a take library
- **Camera rigs and effects:** orbit, dolly zoom, lock-on to a zombie/player, shake
- **Demo tab:** play a Plutonium demo and fly through it with the free camera
- **Zombie spawning** at the crosshair: pick pose/speed/model, reset for the next take
- **Look:** letterbox, vignette, grain, color grade, chromatic aberration, sharpen, bloom, DOF focus selector, depth-key green screen
- **Capture:** smooth fixed-step rendering with motion blur, MP4 via ffmpeg, still capture (F8)
- Overlay UI is **not** recorded into the video

## Requirements
- Windows, Plutonium Black Ops 2 (Zombies)
- Game started in LAN mode with cheats: `-lan -offline +set sv_cheats 1`
- ffmpeg (only for recording video; there is a helper script)

## Install
1. Download **BO2-MVM-Pack.zip** from the [Releases](../../releases) page and unzip it.
2. Run `INSTALL.bat`. It copies everything into `%LOCALAPPDATA%\Plutonium\storage\t6` (your existing `scripts\zm` is backed up once).
3. Start Zombies with `-lan -offline +set sv_cheats 1`, for example:
   `plutonium-bootstrapper-win32.exe t6zm "<your bo2 folder>" -lan -offline +set sv_cheats 1`
4. **Load a map first** (the script only loads at match start), then run `BO2MVM.exe` from `bo2mvm_out`. It injects automatically.
5. Press **INSERT** in game for the menu.
6. For video: run `bo2mvm_out\ffmpeg_install.ps1` once (right-click, Run with PowerShell), or put `ffmpeg.exe` next to `bo2mvm.dll`.

If you already have your own `scripts\zm\main.gsc` (another menu), keep it out of the way or merge it. Only `mvm_zm.gsc` is needed for the MVM tool itself.

## Keys
| Key | Action |
|---|---|
| INSERT | menu |
| F2 | free camera on/off |
| W A S D, Space / Ctrl | fly, up / down |
| Shift / Alt | fast / slow |
| Z / X | roll |
| F4 | freeze game time |
| F6 / F7 | add path key / play-stop path |
| F8 | still capture |
| F9 | record camera path |
| Ctrl+Z / Ctrl+Y | undo / redo path edits |

Everything else is in the menu. Keys and settings are in `settings.ini`.

## Build from source
Needs Visual Studio with the **Desktop development with C++** workload, CMake/Ninja, and the .NET SDK.
- Easy way: put `bo2mvm.zip` (a zip of this repo's folder named `bo2mvm`) next to `build.bat` and run it, see the script.
- Manual: `dll/` builds a 32-bit `bo2mvm.dll` with CMake (`-A Win32`), `injector/` builds `BO2MVM.exe` with `dotnet build -c Release`.
- Technical notes (addresses, how it works, limits): [docs_technical.md](docs_technical.md)

## Layout
| Folder | What |
|---|---|
| `dll/` | in-game DLL (camera, capture, overlay UI) |
| `injector/` | `BO2MVM.exe` launcher/injector |
| `gsc/` | `mvm_zm.gsc` game-side script (zombie spawning, tracking) |
| `tools/` | address verification / tests |

## Known limits
Not tested on every map or Plutonium version. If Plutonium updates `t6zm.exe`, the DLL checks its signatures and disables itself rather than patching unknown memory. No depth-pass export and no audio capture. LAN / offline use only; it will not inject into a game that was not started with `-lan`.

## Credits
- **BO2 MVM tool:** made by **D** together with **Claude** (Anthropic).
- **Centauri zombies menu** (shipped in the release pack, not part of this repo's source): by **@nyli2b**, a revelation fork. All credit for the menu goes to them. The menu is their work and not covered by this repo's license.
- Plutonium and Black Ops 2 belong to their respective owners. This project is not affiliated with Plutonium, Activision or Treyarch.

## License
MIT, see [LICENSE](LICENSE). Do what you want with it.
