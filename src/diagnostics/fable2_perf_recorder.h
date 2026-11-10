// fable2_perf_recorder.h - in-game frame-time recorder: average FPS, 1% /
// 0.1% lows, hitch capture and CSV export for hunting small stutters.
//
// The once-per-frame hook in fps_meter.h (strong override
// MainRenderLoop_82B9CD68, guest main thread) calls on_frame(); each call
// measures the steady_clock delta against the previous one and stores it in a
// kCapacity-entry ring (16384 frames ~= 4.5 min at 60 fps, ~9 min at 30 fps).
// Deltas at least perf_hitch_ms long are hitches: counted, kept (the last
// kRecentHitches) and logged at most once a second.
//
// Threading: exactly one writer - the guest main thread - owns the ring, the
// hitch list and the counters, so the ring itself needs no lock. Every
// kPublishEvery frames the window statistics (avg / median / p99 / p99.9 /
// max over the ring) are copied into a snapshot published under a mutex; that
// copy is what the overlay (UI thread) and the remote-control `perf` handler
// (connection thread) read - never the raw ring. report() only reads that
// copy, but dump_csv() and reset() touch the ring itself, so they run on the
// guest main thread (the console's `perf` commands; dump_csv() restates the
// rule at its definition).
//
// Deltas of at least kGapUs (1 s) are recorded and counted as hitches but
// flagged as gaps and left out of avg/percentiles/max: those are pause holds,
// debugger stops or a window-drag modal loop, not game frame times. They stay
// in the ring and in the CSV so nothing is hidden.
//
// Console: f2 perf [report|reset|dump [path]|overlay on|off] (the `dump`
// default is perf_dump.csv under the game root, which defaults to the exe
// dir). Overlay: register_overlay(drawer) from Fable2App::OnCreateDialogs,
// drawn top-left while perf_overlay = true.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

REXCVAR_DEFINE_INT32(perf_hitch_ms, 50, "Perf",
                     "Frame time in milliseconds at which the frame-time "
                     "recorder counts a hitch (f2 perf report / overlay). "
                     "Frames of 1 s or more are recording gaps (pause, window "
                     "drag) and stay out of the statistics.");
REXCVAR_DEFINE_BOOL(perf_overlay, false, "Perf",
                    "Draw the frame-time overlay (avg FPS, 1% / 0.1% lows, "
                    "frame time, hitches) in the top-left corner.");

namespace fable2::perfrec {

using Clock = std::chrono::steady_clock;

inline constexpr size_t kCapacity = 16384;   // ring: ~4.5 min at 60 fps
inline constexpr size_t kRecentHitches = 8;  // kept for report/overlay/remote
inline constexpr int64_t kGapUs = 1000000;   // >= 1 s: pause/break, not a frame time
inline constexpr uint64_t kPublishEvery = 8;  // frames between snapshot publishes

// Ring entry flags, decided when the frame is recorded.
inline constexpr uint8_t kHitchFlag = 1;
inline constexpr uint8_t kGapFlag = 2;

// One recorded frame boundary.
struct Entry {
  int64_t ts_us;  // steady_clock timestamp of the boundary
  int64_t dt_us;  // delta since the previous boundary
  uint8_t flags;  // kHitchFlag | kGapFlag
};

// A captured hitch (the newest kRecentHitches survive in the snapshot).
struct Hitch {
  uint64_t frame;  // 1-based frame number since reset (matches the CSV)
  int64_t ts_us;
  int64_t dt_us;
  bool gap;  // >= kGapUs: counted, but left out of the stats
};

// What the readers see: a copy published under mu(), never the ring itself.
struct Snapshot {
  uint64_t frames = 0;   // frames recorded since reset
  uint64_t window = 0;   // ring entries the stats below cover (gaps excluded)
  uint64_t hitches = 0;  // dt >= perf_hitch_ms (gaps included)
  uint64_t gaps = 0;     // of those, >= kGapUs and excluded from the stats
  int64_t t0_us = 0;     // first recorded boundary (hitch times are relative to it)
  double avg_fps = 0.0;
  double avg_ms = 0.0;
  double median_ms = 0.0;
  double p99_ms = 0.0;   // 99th percentile of the window = 1% low frame time
  double p999_ms = 0.0;  // 99.9th percentile of the window = 0.1% low frame time
  double max_ms = 0.0;
  std::vector<Hitch> recent;  // newest last
};

// Writer state: guest main thread ONLY (the MainRenderLoop hook + the
// console's perf report/reset/dump commands all run there).
struct Writer {
  bool primed = false;
  Clock::time_point last{};
  uint64_t frames = 0;  // entries written since reset
  size_t head = 0;      // next slot to write
  size_t count = 0;     // valid entries (<= kCapacity)
  uint64_t hitches = 0;
  uint64_t gaps = 0;
  uint64_t since_publish = 0;
  Clock::time_point last_warn{};
  int64_t t0_us = 0;
  std::deque<Hitch> recent;
  Entry ring[kCapacity];

  // Metadata only; the ring contents are hidden by `count`.
  void clear() {
    primed = false;
    last = Clock::time_point{};
    frames = 0;
    head = 0;
    count = 0;
    hitches = 0;
    gaps = 0;
    since_publish = 0;
    last_warn = Clock::time_point{};
    t0_us = 0;
    recent.clear();
  }
};

inline Writer& W() {
  static Writer w{};
  return w;
}

inline std::mutex& mu() { static std::mutex m; return m; }
inline Snapshot& published() { static Snapshot s; return s; }  // caller holds mu()

// A copy of the published stats; safe from any thread (overlay, remote `perf`).
inline Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(mu());
  return published();
}

// Entry `i` of the ring, oldest first (0 .. count - 1).
inline const Entry& ring_at(const Writer& w, size_t i) {
  return w.ring[(w.head + kCapacity - w.count + i) % kCapacity];
}

// Recompute the window stats from the ring and publish them. Guest main
// thread; only the Snapshot assignment is under the mutex - the copy and the
// selection happen outside it. Called every kPublishEvery frames and from
// reset(). Percentiles use nth_element rather than a full sort: this runs on
// the game thread, and a 16k sort would add a visible millisecond spike to
// the frame right after each publish (the recorder must not measure itself).
inline void publish_stats() {
  const Writer& w = W();
  std::vector<int64_t> dts;
  dts.reserve(w.count);
  int64_t sum = 0, max_us = 0;
  for (size_t i = 0; i < w.count; ++i) {
    const Entry& e = ring_at(w, i);
    if (e.flags & kGapFlag) continue;  // pause/break deltas are not frame times
    dts.push_back(e.dt_us);
    sum += e.dt_us;
    if (e.dt_us > max_us) max_us = e.dt_us;
  }
  // Nearest-rank order statistic over the window (permille: 500 = median).
  // Each call re-selects in place; that is fine, the vector is only a copy.
  auto at = [&dts](int permille) -> int64_t {
    const size_t n = dts.size();
    if (n == 0) return 0;
    size_t rank = (static_cast<size_t>(permille) * n + 999) / 1000;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    const size_t idx = rank - 1;
    std::nth_element(dts.begin(), dts.begin() + static_cast<std::ptrdiff_t>(idx), dts.end());
    return dts[idx];
  };
  Snapshot s;
  s.frames = w.frames;
  s.window = dts.size();
  s.hitches = w.hitches;
  s.gaps = w.gaps;
  s.t0_us = w.t0_us;
  if (!dts.empty()) {
    s.avg_ms = (static_cast<double>(sum) / static_cast<double>(dts.size())) / 1000.0;
    s.avg_fps = s.avg_ms > 0.0 ? 1000.0 / s.avg_ms : 0.0;
    s.median_ms = at(500) / 1000.0;
    s.p99_ms = at(990) / 1000.0;
    s.p999_ms = at(999) / 1000.0;
    s.max_ms = static_cast<double>(max_us) / 1000.0;
  }
  s.recent.assign(w.recent.begin(), w.recent.end());
  std::lock_guard<std::mutex> lock(mu());
  published() = std::move(s);
}

// Record one frame. Guest main thread only (called from the MainRenderLoop
// hook); the first call just primes the clock.
inline void on_frame() {
  Writer& w = W();
  const Clock::time_point now = Clock::now();
  if (!w.primed) {
    w.primed = true;
    w.last = now;
    return;
  }
  const int64_t dt =
      std::chrono::duration_cast<std::chrono::microseconds>(now - w.last).count();
  w.last = now;
  if (dt <= 0) return;  // same-tick frame (or a clock glitch): nothing to store
  const int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                         now.time_since_epoch())
                         .count();
  if (w.frames == 0) w.t0_us = ts;
  int64_t thr_ms = REXCVAR_GET(perf_hitch_ms);
  if (thr_ms < 1) thr_ms = 1;
  uint8_t flags = 0;
  if (dt >= thr_ms * 1000) {
    flags |= kHitchFlag;
    ++w.hitches;
    const bool gap = dt >= kGapUs;
    if (gap) {
      flags |= kGapFlag;
      ++w.gaps;
    }
    w.recent.push_back(Hitch{w.frames + 1, ts, dt, gap});
    if (w.recent.size() > kRecentHitches) w.recent.pop_front();
    if (now - w.last_warn >= std::chrono::seconds(1)) {  // one warning per second
      w.last_warn = now;
      REXSYS_WARN("[perf] hitch #{}: frame {} dt={:.1f}ms (>= {}ms){}",
                  w.hitches, w.frames + 1, dt / 1000.0, thr_ms,
                  gap ? ", gap: left out of the stats" : "");
    }
  }
  Entry& e = w.ring[w.head];
  e.ts_us = ts;
  e.dt_us = dt;
  e.flags = flags;
  w.head = (w.head + 1) % kCapacity;
  if (w.count < kCapacity) ++w.count;
  ++w.frames;
  if (w.frames == 1 || ++w.since_publish >= kPublishEvery) {
    w.since_publish = 0;
    publish_stats();
  }
}

// Human-readable stats as a multi-line string (the console prints one `perf`
// line per line here). Reads only the published snapshot, so it is safe from
// any thread; it is up to kPublishEvery frames behind the ring.
inline std::string report() {
  const Snapshot s = snapshot();
  if (s.frames == 0) return "no frames recorded yet";
  const double low1 = s.p99_ms > 0.0 ? 1000.0 / s.p99_ms : 0.0;
  const double low01 = s.p999_ms > 0.0 ? 1000.0 / s.p999_ms : 0.0;
  std::string last;
  for (const Hitch& h : s.recent) {
    char hb[80];
    std::snprintf(hb, sizeof(hb), "%sf%llu +%.1fms @%.1fs%s", last.empty() ? "" : ", ",
                  static_cast<unsigned long long>(h.frame), h.dt_us / 1000.0,
                  static_cast<double>(h.ts_us - s.t0_us) / 1e6, h.gap ? " (gap)" : "");
    last += hb;
  }
  if (last.empty()) last = "none";
  char buf[768];
  std::snprintf(buf, sizeof(buf),
                "frames %llu, stats over the last %llu (gaps %llu excluded)\n"
                "avg %.2f fps / %.2f ms, median %.2f ms, max %.2f ms\n"
                "1%% low %.2f fps (%.2f ms), 0.1%% low %.2f fps (%.2f ms)\n"
                "hitches %llu (>= %d ms), last: %s",
                static_cast<unsigned long long>(s.frames),
                static_cast<unsigned long long>(s.window),
                static_cast<unsigned long long>(s.gaps), s.avg_fps, s.avg_ms, s.median_ms,
                s.max_ms, low1, s.p99_ms, low01, s.p999_ms,
                static_cast<unsigned long long>(s.hitches),
                static_cast<int>(REXCVAR_GET(perf_hitch_ms)), last.c_str());
  return buf;
}

// Write the ring as CSV rows "frame,timestamp_us,dt_us,hitch". Guest main
// thread only: it reads the ring while the same thread is the writer, so no
// lock is needed - from any other thread the writer could be mid-update (use
// report()/snapshot() there instead). Returns the row count (0 when nothing
// was recorded) or -1 when the file cannot be written.
inline int64_t dump_csv(const std::filesystem::path& path) {
  namespace fs = std::filesystem;
  Writer& w = W();
  std::error_code ec;
  if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::trunc);
  if (!f) return -1;
  f << "frame,timestamp_us,dt_us,hitch\n";
  const uint64_t first = w.frames - w.count + 1;  // frame number of the oldest entry
  for (size_t i = 0; i < w.count; ++i) {
    const Entry& e = ring_at(w, i);
    f << (first + i) << ',' << e.ts_us << ',' << e.dt_us << ','
      << ((e.flags & kHitchFlag) ? 1 : 0) << '\n';
  }
  return static_cast<int64_t>(w.count);
}

// Clear the ring, the hitch list and the published stats. Guest main thread
// (console `perf reset`); the clock primes again on the next frame, so the
// reset itself does not show up as a frame time.
inline void reset() {
  W().clear();
  publish_stats();
}

// The overlay: frame-time numbers top-left, drawn over the guest frame while
// perf_overlay is on. Same self-retaining shape as fable2_deadbeef_overlay.h
// (drawn from the foreground draw list, no guest text involved).
class PerfOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlayDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!REXCVAR_GET(perf_overlay)) return;
    const Snapshot s = snapshot();
    if (s.frames == 0) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    const double low1 = s.p99_ms > 0.0 ? 1000.0 / s.p99_ms : 0.0;
    const double low01 = s.p999_ms > 0.0 ? 1000.0 / s.p999_ms : 0.0;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "perf  %.1f fps   1%% low %.1f  0.1%% low %.1f\n"
                  "frame %.2f ms avg  %.2f med  %.2f max\n"
                  "hitches %llu (>= %d ms)  window %llu",
                  s.avg_fps, low1, low01, s.avg_ms, s.median_ms, s.max_ms,
                  static_cast<unsigned long long>(s.hitches),
                  static_cast<int>(REXCVAR_GET(perf_hitch_ms)),
                  static_cast<unsigned long long>(s.window));
    const ImVec2 pos(8.0f, 8.0f);
    const ImVec2 size = ImGui::CalcTextSize(buf);
    dl->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 4.0f),
                      ImVec2(pos.x + size.x + 4.0f, pos.y + size.y + 4.0f),
                      IM_COL32(0, 0, 0, 160));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, IM_COL32(140, 255, 140, 255),
                buf);
  }
};

// Called from Fable2App::OnCreateDialogs (fable_2_app.h) to register the
// overlay; the dialog retains itself, like the deadbeef one.
inline void register_overlay(rex::ui::ImGuiDrawer* drawer) {
  (void)new PerfOverlayDialog(drawer);  // dialog retains itself
}
inline void register_overlay(rex::ui::ImGuiDrawer& drawer) { register_overlay(&drawer); }

}  // namespace fable2::perfrec
