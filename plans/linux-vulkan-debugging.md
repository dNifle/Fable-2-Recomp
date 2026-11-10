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

## 3. Stutters (ROOT CAUSE FOUND, FIX LANDED)

The "small stutters from time to time" correlate 1:1 with runtime
pipeline-state creation bursts (clean window: 2 creations; stutter window:
34, with recurring vertex shaders across states). `vkCreateGraphicsPipelines`
was called with `VK_NULL_HANDLE`, so the driver recompiled every shader
permutation per pipeline state. Fixed: a persistent `VkPipelineCache`
(`cache/shaders/shareable/<title>.vkpc`, merged at startup, saved during the
run since the app hard-exits) - see thirdparty/rexglue-sdk-linux.patch.

Related policy (user-tunable, explains the slow cold boot too):
- `async_pipeline_wait = true` (default): draws block on in-flight pipeline
  creation ("a short stall the first time a pipeline is needed") = the
  stutters. false = objects pop in late instead.
- `vulkan_async_skip_incomplete_frames = true` (default): placeholder frames
  are not presented = the frozen/crawling cold boot. false = brief flashes.
- Videos render fluidly because playback uses a small stable pipeline set.

Verified: create -> periodic save (14.6 KB after a menu session) -> "warmed
from ... .vkpc" merge on the next run. Next: measure gameplay hitches with
`perf` on a warm and a cold cache; if first-encounter hitches stay
perceptible, pre-translate the game's shader set during loading screens.

## 3b. Boot polish (OPEN)

- The window opens 1600x900 and only later becomes fullscreen; the guest
  reaches the intro videos before the transition completes ("window slowly
  stretches", videos before fullscreen). Fix: open the window fullscreen
  from the start.
- Cold boot has no visual feedback while shader translation + pipeline
  prewarm run.

## 4. Performance improvements (OPEN)

After #3 identifies the real costs. Candidate already known: port the
D3D12-only CPU shortcuts (`fable2_terrain_tess` / `fable2_skinning`,
`skip_depth_clear_transfers`) to the Vulkan backend.
