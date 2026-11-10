# Linux/Vulkan debugging session (in progress)

Started 2026-11-10 after the first Linux/Vulkan smoke test; refined by the
maintainer's own testing (performance is already clearly better than the
Windows build through Proton).

## 1. Presentation: 16:9 output in fullscreen (DONE)

Bug: on non-16:9 fullscreen (and borderless) the game filled the whole
ultrawide and showed content the 16:9 framing keeps off-screen (the title
screen's side art). Windowed at 16:9 was already correct.

Root cause: `src/core/fable2_fit_window.h` writes the *window* aspect into
the guest `.rdata` aspect floats (0x8209FE7C / 0x8209FDF4), which widens the
GUI camera canvas (see docs/FIT_WINDOW.md "title screen" note), and forces
`present_letterbox=false` so the presenter stretches the frame wall-to-wall.

Fix (implemented): fullscreen/borderless output is pinned to 16:9 with
letterbox bars — `fitwindow::PinOutput16x9()` in `src/core/fable2_fit_window.h`,
applied by `ApplySize`/`SyncPresenterMode` and kept in sync across fullscreen
toggles by `ui::TickVideo` (which also keeps video letterboxing). Windowed
windows keep the fit-window behavior. Overrides:
- `experimental_ultrawide_fit = true` (cvar / `fable_2.toml` key) restores the
  wide framing in fullscreen (experimental).
- `f2 ws aspect <n>` (console, deliberate) always wins over the pin.

To verify: windowed 16:9 unchanged; borderless+exclusive fullscreen on the
ultrawide show a centered 16:9 image with bars; `experimental_ultrawide_fit`
brings the old look back.

## 2. In-game performance measurement (DONE)

"Measure actual ingame performance" - frame times, not just the F3 FPS
(which is a 500 ms mean of guest XE_SWAP intervals). New
`src/diagnostics/fable2_perf_recorder.h`, sampled from the per-guest-frame
main-loop hook:

- Stats: average FPS, median / 99th-percentile (1% low) / 99.9th-percentile
  (0.1% low) frame times, max, frame count.
- Hitch capture: frames over `perf_hitch_ms` (cvar, default 50 ms) are logged
  (rate-limited) and counted.
- Console `perf report|reset|dump [path]|overlay on|off` (works while paused);
  `perf dump` writes per-frame CSV (`frame,timestamp_us,dt_us,hitch`).
- Optional on-screen overlay (`perf_overlay` cvar).
- Debug builds: `perf` command on the remote-control TCP channel.

## 3. Stutters (OPEN)

"Small stutters from time to time" - use the tooling from #2 to quantify and
locate them. Suspects to check once we have CSV numbers:
- Vulkan pipeline/state creation at runtime (the pre-baked SPIR-V set is
  smaller than the D3D12 one; tessellation/FXAA GLSL is compiled at runtime
  via glslang - docs/main_menu_crash_fix.md notes the slower path).
- Texture streaming / guest-to-host uploads (see the hero/dog texture readback
  work in plans/hero-dog-texture-readback.md).
- Guest vblank pacing / present gating (docs/FPS_CAP_INVESTIGATION.md).

## 4. Performance improvements (OPEN)

After #3 identifies the real costs. Candidate already known: port the
D3D12-only CPU shortcuts (`fable2_terrain_tess` / `fable2_skinning`,
`skip_depth_clear_transfers`) to the Vulkan backend.
