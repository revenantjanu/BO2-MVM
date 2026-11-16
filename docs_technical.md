# BO2 MVM (Plutonium T6, LAN only)

Movie-maker / cinematic tool: freecam with roll and FOV, slow motion / freeze,
camera paths on a timeline (linear or spline), and frame capture to ffmpeg or TGA. ImGui menu in game.

Stack: C++ DLL (`dll/`, DirectX 11 + ImGui, 32 bit) and a C# launcher/injector (`injector/`, x86).

## Status — read this first
* **Not compiled or run.** The workspace I built this in has no Windows compiler and cannot run the game.
  `dll/src/camera.h` (camera, spline, path files) has a host-side test (`tools/test_camera.cpp`) that passes,
  and `core.cpp` passes a syntax check, but `overlay.cpp`, `capture.cpp` and the C# project have never been built.
  Expect to fix a few compile errors on the first build. Send me the compiler output and I'll patch them in one go.
* The game addresses are real: taken from your `t6zm.exe` (32 bit, image base 0x400000) and checked with
  `tools/verify_addresses.py` (all three signatures match exactly once).
* LAN only is enforced twice: the launcher refuses to inject unless the game command line contains `-lan`,
  and the DLL checks the same before it hooks anything.

## Build
DLL (must be **Win32 / x86**):
```
cd dll
cmake -S . -B build -A Win32
cmake --build build --config Release        # -> build/Release/bo2mvm.dll
```
Launcher (x86):
```
cd injector
dotnet build -c Release                     # -> bin/Release/net48/BO2MVM.exe
```
Put `bo2mvm.dll` and `BO2MVM.exe` in the same folder. For capture, install ffmpeg and either put `ffmpeg.exe`
in that folder or set `ffmpeg_path` in `settings.ini`.

## Use
1. Start Plutonium in LAN mode with cheats on (your `lan_launch_fixed.bat` already does: `-lan -offline +set sv_cheats 1`).
2. Load into a map, then run `BO2MVM.exe` (auto-injects) — or press **Inject**.
3. In game:

| Key | Action |
|---|---|
| INSERT | menu (hotkeys work without it) |
| F2 | freecam on/off |
| W A S D, Space / Ctrl | fly, up / down (mouse look = normal game look) |
| Shift / Alt | fast / slow |
| Z / X | roll left / right |
| F4 | freeze game time |
| F6 / F7 / F8 | add path key / play-stop path / clear path |

Time scale, FOV, path editing (times, delete, interpolation, loop, scrub), save/load paths, capture settings
and start/stop are in the menu. All keys, speeds, capture fps/crf etc. are in `settings.ini`
(plain key=value; use the menu's *Save settings.ini*).

Path files (`paths/<name>.bo2path`) are plain text, one key per line
(`cam time_ms x y z pitch yaw roll fov`).
Output goes to `captures/`.

## How it works
| Piece | Address in your t6zm.exe | Notes |
|---|---|---|
| `CG_CalcViewValues` | `0x7D5360` | hooked; after the original runs we overwrite the view |
| cg_t pointer | `0x113F18C` | read from the function's own code |
| `refdef.vieworg` / `viewaxis` | `cg+0x4D8C4` / `cg+0x4D8D4` | seen in `CalcViewValues` and `CG_OffsetThirdPersonView` |
| `AnglesToAxis` | `0x4D01F0` | used to write roll-capable axes |
| `Cbuf_AddText(int, const char*)` | `0x5BDF70` | the engine's own call for `disconnect\n`, `vid_restart\n`... |

Everything else (timescale, FOV, HUD, max fps) goes through console commands, so no more offsets are needed:
`com_timescale`, `cg_fov`, `cg_draw2D`, `cg_drawGun`, `cg_thirdPerson`, `com_maxfps`.
Rendering hook: `IDXGISwapChain::Present` / `ResizeBuffers` (BO2 uses D3D11).

## Known limits / next steps
* Capture is real-time (frames are paced to `capture_fps`, duplicated if the game is slower); it is not a
  frame-exact offline renderer. `com_maxfps` is set to the capture fps while recording.
* While the menu is open the game still receives mouse movement; the camera is held still and re-synced on close.
  If the game captures the mouse so the menu cursor cannot move, use keyboard navigation (arrows / Enter).
* Mouse look is the game's own view angles (offset-free). The player does not move while flying (movement
  key-downs are swallowed), but turns with the mouse.
* Not in v1 (as agreed): demo record/replay, FX placement, bone-attached FX, visuals.ini features (fog, sun,
  SSAO, DOF, wireframe...). DOF dvars (`r_dof_*`) and fog (`r_fog`) exist in your binary, so adding them as
  console-driven sliders is the natural next step; demo recording needs entity/state offsets and a separate pass.
* If Plutonium updates `t6zm.exe`: run `tools/verify_addresses.py <t6zm.exe>`. The DLL also searches `.text` for the
  signatures and disables itself rather than patching unverified memory.
