# Endfield Uncensored — ReShade add-on

A ReShade add-on port of [DynamiByte/Endfield-Uncensored](https://github.com/DynamiByte/Endfield-Uncensored).
It removes *Arknights: Endfield*'s camera-pitch transparency dither without the separate EFU launcher/injector:
ReShade loads it, and it works with both DX11 and Vulkan.

## How it works

Same patch as upstream `src/dll.zig`:

1. Wait for `GameAssembly.dll` and for IL2CPP to finish loading `Gameplay.Beyond.dll`.
2. Look up `Beyond.Gameplay.View.CameraMono._ProcessDitherByPitch` and `ForceClearDither` through the IL2CPP exports.
3. Hook `_ProcessDitherByPitch` (MinHook) and call `ForceClearDither` right after it whenever the patch is enabled.

What's different from EFU:

- No launcher, no admin rights, no process injection — ReShade loads it as an add-on.
- The upstream `OneChannelLC.dll` launch-check hook is **not** included. EFU needs it because it starts `Endfield.exe`
  directly; with ReShade you launch the game the normal way, so it isn't needed.
- Toggle state is saved in `ReShade.ini` under `[EndfieldUncensored]`.

## Install

1. Install ReShade **with full add-on support** for `Endfield.exe` (pick DirectX 10/11/12 or Vulkan to match how you run the game).
2. Put `EndfieldUncensored.addon64` next to `Endfield.exe` (or in the add-on search path set in ReShade's settings).
3. Launch the game normally.
4. Open the ReShade overlay → **Add-ons** tab → *Endfield Uncensored* to see the status and the toggle.

**Alt+F12** toggles the patch in game (only while the game window is focused).

## Build (Windows, Visual Studio 2022)

```
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/EndfieldUncensored.addon64`.

All dependencies are vendored in `deps/`:
- `deps/reshade` — ReShade add-on SDK headers (BSD-3-Clause), from crosire/reshade
- `deps/imgui` — Dear ImGui `imgui.h` 1.92.5 (19250), the exact version ReShade's add-on API expects (MIT)
- `deps/minhook` — MinHook (BSD-2-Clause)

If a future ReShade release bumps its ImGui version, the add-on will refuse to load; replace `deps/reshade` and
`deps/imgui/imgui.h` with the matching versions and rebuild.

## Notes

- If a game update renames or removes those `CameraMono` methods, the status line reports it and nothing is patched.
- Modifying an online game at runtime may break its Terms of Service. ReShade's add-on build is meant for games without
  anti-cheat. Use at your own risk, the same as upstream.

## License

AGPL-3.0, same as upstream (this is a derivative work). See `LICENSE.txt`.
# EFU-Reshade
