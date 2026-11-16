# BO2 MVM

![status](https://img.shields.io/badge/status-work%20in%20progress-orange) ![platform](https://img.shields.io/badge/platform-Plutonium%20T6-blue) ![mode](https://img.shields.io/badge/mode-LAN%20only-lightgrey) ![license](https://img.shields.io/badge/license-MIT-green)

**A movie-maker tool for Black Ops 2 Zombies on Plutonium (T6).** Free camera, keyframed paths, posable zombies, post effects and video capture.

[Download](../../releases/latest) · [Install](#install) · [Controls](#controls) · [Build](#building-from-source) · [Credits](#credits)

---

BO2 MVM is built for putting together cinematics and trickshot edits in Zombies. It runs inside the game while you play, so you can fly around, set up a shot, pose a few zombies, and record it.

**This is a work in progress.** Parts of it are untested, some things are rough, and some things are unfinished. The full source is in this repo so you can fix it, change it, or build on it however you like.

## What it does

| | |
|---|---|
| **Camera** | Free camera with roll and FOV. Keyframed paths with per-key ease and speed, spline or linear, undo/redo, scrubbing and speed ramps. One-key path recording (F9) with a countdown, smoothing and a take library. Orbit, dolly zoom, lock-on to a zombie or the player, camera shake. A demo tab for flying through saved Plutonium demos. |
| **Zombies** | Spawn zombies at your crosshair and choose their pose, speed and model. Reset everything to the same positions for the next take. |
| **Look** | Letterbox, vignette, grain, color grade, chromatic aberration, sharpen and bloom. Depth of field with a focus selector (you, a zombie, lock-on or manual). Depth-key green screen. |
| **Capture** | Fixed-step rendering with motion blur, written to MP4 through ffmpeg. Stills with F8. The overlay never shows up in the recording. |

## Install

You need Windows and Plutonium with Black Ops 2 installed.

| Step | What to do |
|---|---|
| 1 | Download **BO2-MVM-Pack.zip** from the [Releases](../../releases) page and unzip it. |
| 2 | Run **INSTALL.bat**. It copies everything into your Plutonium t6 folder and backs up your existing scripts\zm folder first. |
| 3 | Start Zombies in LAN mode with cheats on (command below). |
| 4 | Load into a map. The script only loads when a match starts. |
| 5 | Run **BO2MVM.exe** from the bo2mvm_out folder. It injects itself. |
| 6 | Press **INSERT** in game to open the menu. |

Launch command for step 3:

```
plutonium-bootstrapper-win32.exe t6zm "your BO2 folder" -lan -offline +set sv_cheats 1
```

For video capture, run `ffmpeg_install.ps1` once (right click, Run with PowerShell) or put `ffmpeg.exe` next to `bo2mvm.dll`.

If you already run your own `scripts\zm\main.gsc` (another menu), merge it or keep it out of the way. The tool itself only needs `mvm_zm.gsc`.

## Controls

| Key | Action |
|---|---|
| `INSERT` | Open the menu |
| `F2` | Free camera on/off |
| `W` `A` `S` `D`, `Space`, `Ctrl` | Fly, up, down |
| `Shift` / `Alt` | Faster / slower |
| `Z` / `X` | Roll |
| `F4` | Freeze game time |
| `F6` / `F7` | Add path key / play and stop path |
| `F8` | Capture a still |
| `F9` | Record a camera path |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo path edits |

Everything else is in the menu. Key bindings and defaults live in `settings.ini`.

## Building from source

You need Visual Studio with the Desktop development with C++ workload, CMake, Ninja and the .NET SDK.

| Folder | What it is | How to build |
|---|---|---|
| `dll/` | The in-game DLL (camera, capture, overlay) | `cmake -S . -B build -A Win32` then `cmake --build build --config Release` |
| `injector/` | `BO2MVM.exe`, the launcher and injector | `dotnet build -c Release` |
| `gsc/` | `mvm_zm.gsc`, the game-side script | Copy to `scripts\zm` |
| `tools/` | Address check and camera test | |

`build.bat` does the DLL and injector in one go if you put a zip of this repo's `bo2mvm` folder next to it. Notes on how it hooks the game and the addresses it uses are in [docs_technical.md](docs_technical.md).

## Limitations

Not tested on every map or Plutonium build. If `t6zm.exe` changes, the DLL checks its signatures and turns itself off instead of patching memory it can't verify. There is no depth-pass export and no audio capture. It will not inject into a game that wasn't started with `-lan`.

## Is this safe?

BO2 MVM works by injecting a DLL into the game, which is also how cheats work, so some antivirus tools may flag it. That is why the full source is here. Read it, or build it yourself with `build.bat` and use your own files instead of the prebuilt ones.

Everything it does outside the game window: it starts `ffmpeg.exe` to encode your recordings, it opens folders in Explorer from the menu, and only if you use the ffmpeg download button or script, it downloads ffmpeg from GitHub (BtbN) or gyan.dev. It makes no other network connections, and it will not run unless the game was started with `-lan`. Use it offline only. Injecting into online games can get you banned.

## Credits

| | |
|---|---|
| **BO2 MVM tool** | D and Claude (Anthropic) |
| **Centauri zombies menu** (in the release pack) | @nyli2b. All credit for the menu is theirs. It is their work and is not covered by this repo's license. |
| **Dear ImGui** | Omar Cornut and contributors (MIT) |

Plutonium and Black Ops 2 belong to their owners. This project isn't affiliated with Plutonium, Activision or Treyarch.

## License

MIT. See [LICENSE](LICENSE).
