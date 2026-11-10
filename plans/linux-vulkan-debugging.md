# Linux/Vulkan debugging session (planned)

Agenda collected on 2026-11-10 after the first Linux/Vulkan smoke test
(Debug + Release builds boot the game to the title screen under Wayland +
RADV on an ultrawide display). Nothing here is worked on yet; this is the
list for the dedicated debugging/performance session.

## 1. Presentation: game must render 16:9 only ("side art" bug)

Observed: on the ultrawide host display the title screen shows decorative
side panels (winter scene left, forest right) filling the space around the
16:9 game image. Expected: the game renders 16:9 only; the image is normally
panning through the center - no side art.

Notes from the first run (`logs/fable_2_001.log`):
- The `[ui-layout] [fitwin]` dump runs a full main-menu layout pass, so the
  guest UI is being re-laid-out for the non-16:9 output - suspect this and/or
  the present path (`present_letterbox` cvar, "Enable letterboxing for
  non-native aspect ratios") before blaming the Vulkan backend itself.
- Reproduce windowed at a 16:9 size first (e.g. 1280x720) to establish the
  correct baseline, then fullscreen on the ultrawide.
- Compare against the Windows/D3D12 build (known-good behavior on this same
  machine via Wine) at the same output resolution.

## 2. Performance improvements

To profile once the presentation is correct. Relevant existing tooling:
- `fable2_func_trace` + `tools/run_profiler.cmd` (function trace flow),
  F3 FPS counter in-game, `fps_probe`, `fable2_stall_dump`.
- Release build already uses `-O3`; the SDK runtime is built Release + `-g`.
- Known D3D12-only CPU shortcuts that have no Vulkan counterpart yet
  (candidates to port): `fable2_terrain_tess` / `fable2_skinning` CPU paths
  and `skip_depth_clear_transfers` (see `docs/main_menu_crash_fix.md`,
  `thirdparty/rexglue-sdk/src/graphics/d3d12/`).
