// Headless benchmark for the Horizon File Browser.
//
// Works without any Wayland compositor: paints into offscreen cairo image
// surfaces using the REAL production draw path (eh::file_browser::paint()).
// Reports per-view-mode / per-resolution paint cost (total + the app's own
// >=1 ms phase breakdown via the resize_phase_samples sink), directory
// scan cost, and memory (VMRSS / VmHWM / PSS + anon / file / shmem /
// private/shared from smaps_rollup) at every milestone.
//
// Usage:
//   bench_paint [--entries N] [--dir PATH] [--frames N] [--warmup N]
//               [--size WxH]... [--mode list|grid|tree|compact]...
//
// Build: meson compile -C build-debug bench_paint
// Run:   build-debug/bench_paint --size 1600x900 --frames 40

#include "app/file_browser/app.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fb = eh::file_browser;

// ── /proc memory snapshot ────────────────────────────────────────────

struct MemSnap {
  int64_t vm_rss_kb = 0;    // VmRSS (smaps)
  int64_t vm_hwm_kb = 0;    // VmHWM (peak RSS)
  int64_t vm_size_kb = 0;   // VmSize (virtual)
  int64_t pss_kb = 0;       // proportional set size
  int64_t rss_anon = 0;
  int64_t rss_file = 0;
  int64_t rss_shmem = 0;
  int64_t priv_clean = 0;
  int64_t priv_dirty = 0;
  int64_t sh_clean = 0;
  int64_t sh_dirty = 0;
  int64_t swap_kb = 0;
};

static double kb_to_mib(int64_t kb) { return static_cast<double>(kb) / 1024.0; }

static MemSnap read_mem() {
  MemSnap m;
  std::ifstream st("/proc/self/status");
  std::string line;
  while (std::getline(st, line)) {
    if (line.rfind("VmRSS:", 0) == 0) m.vm_rss_kb       = std::atoll(line.c_str() + 6);
    else if (line.rfind("VmHWM:", 0) == 0) m.vm_hwm_kb  = std::atoll(line.c_str() + 6);
    else if (line.rfind("VmSize:", 0) == 0) m.vm_size_kb = std::atoll(line.c_str() + 7);
  }
  std::ifstream sr("/proc/self/smaps_rollup");
  while (std::getline(sr, line)) {
    if (line.rfind("Pss:", 0) == 0) m.pss_kb        = std::atoll(line.c_str() + 4);
    else if (line.rfind("RssAnon:", 0) == 0) m.rss_anon = std::atoll(line.c_str() + 8);
    else if (line.rfind("RssFile:", 0) == 0) m.rss_file = std::atoll(line.c_str() + 8);
    else if (line.rfind("RssShmem:", 0) == 0) m.rss_shmem = std::atoll(line.c_str() + 9);
    else if (line.rfind("Pss_Anon:", 0) == 0 && m.rss_anon == 0)
      m.rss_anon   = std::atoll(line.c_str() + 9);
    else if (line.rfind("Pss_File:", 0) == 0 && m.rss_file == 0)
      m.rss_file   = std::atoll(line.c_str() + 9);
    else if (line.rfind("Pss_Shmem:", 0) == 0 && m.rss_shmem == 0)
      m.rss_shmem  = std::atoll(line.c_str() + 10);
    else if (line.rfind("Private_Clean:", 0) == 0) m.priv_clean = std::atoll(line.c_str() + 14);
    else if (line.rfind("Private_Dirty:", 0) == 0) m.priv_dirty = std::atoll(line.c_str() + 14);
    else if (line.rfind("Shared_Clean:", 0) == 0) m.sh_clean   = std::atoll(line.c_str() + 13);
    else if (line.rfind("Shared_Dirty:", 0) == 0) m.sh_dirty   = std::atoll(line.c_str() + 13);
    else if (line.rfind("Swap:", 0) == 0) m.swap_kb     = std::atoll(line.c_str() + 5);
  }
  return m;
}

static void print_mem(const char* label, const MemSnap& m) {
  std::printf(
      "  %-28s RSS %8.2f MiB  HWM %8.2f MiB  VSZ %8.2f MiB  PSS %8.2f MiB"
      "  anon %7.2f  file %7.2f  shmem %6.2f  priv %7.2f  shared %7.2f  swap %6.2f MiB\n",
      label, kb_to_mib(m.vm_rss_kb), kb_to_mib(m.vm_hwm_kb),
      kb_to_mib(m.vm_size_kb), kb_to_mib(m.pss_kb), kb_to_mib(m.rss_anon),
      kb_to_mib(m.rss_file), kb_to_mib(m.rss_shmem),
      kb_to_mib(m.priv_clean + m.priv_dirty), kb_to_mib(m.sh_clean + m.sh_dirty),
      kb_to_mib(m.swap_kb));
}

// ── synthetic directory listing ──────────────────────────────────────

static void synthesize_entries(fb::AppState& app, int n, const std::string& base) {
  auto& tab = app.cur_tab();
  tab.current_path = base;
  tab.entries.clear();
  tab.entries.reserve(static_cast<size_t>(n));
  static const char* kNames[] = {"readme.md", "build.sh",    "icon.svg",  "notes.txt",
                                 "data.bin",  "archive.tar", "blob.dat",  "output.log",
                                 "gen.c",     "doc.pdf",     "clip.mp4",  "photo.jpg",
                                 "song.mp3",  "theme.json",  "module.so", "app.out"};
  static const fb::FileType kTypes[] = {
      fb::FileType::Markdown, fb::FileType::Executable, fb::FileType::Image,
      fb::FileType::Text,     fb::FileType::File,       fb::FileType::Archive,
      fb::FileType::Code,     fb::FileType::Document,   fb::FileType::Code,
      fb::FileType::Document, fb::FileType::Video,      fb::FileType::Image,
      fb::FileType::Audio,    fb::FileType::Code,       fb::FileType::Executable,
      fb::FileType::Executable};
  for (int i = 0; i < n; ++i) {
    fb::FileEntry e;
    char nm[96];
    std::snprintf(nm, sizeof(nm), "item_%06d_%s", i, kNames[i % 16]);
    e.name = nm;
    e.path = base + "/" + nm;
    e.type = (i % 12 == 0) ? fb::FileType::Folder : kTypes[i % 16];
    e.is_dir = (e.type == fb::FileType::Folder);
    e.size = (i * 7919ull) % 0x100000000ull;
    e.modified_sec = 1700000000ll - (i * 97);
    e.mode = e.is_dir ? 0755u : 0644u;
    e.readable = true;
    e.writable = true;
    e.is_hidden = (i % 20 == 0);
    e.extension = "bin";
    if (e.type == fb::FileType::Folder) e.extension.clear();
    tab.entries.push_back(std::move(e));
  }
  tab.visible_entries.clear();
  tab.visible_entries.reserve(tab.entries.size());
  for (int i = 0; i < static_cast<int>(tab.entries.size()); ++i) {
    if (!app.show_hidden && tab.entries[i].is_hidden) continue;
    tab.visible_entries.push_back(i);
  }
  tab.selected_idx = -1;
  tab.hover_idx = -1;
  tab.scroll_px = 0;
  tab.scroll_smooth_current = 0.0;
  tab.scroll_smooth_target = 0.0;
}

static bool load_real_dir(fb::AppState& app, const std::string& path) {
  app.cur_tab().current_path = path;
  app.startup_loading = false;
  auto t0 = std::chrono::steady_clock::now();
  fb::reload_dir(app);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!app.scan_ready_flag.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (app.scan_ready_flag.load(std::memory_order_acquire))
    fb::apply_scan_result(app);
  double ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count();
  std::printf("  directory scan         %8.1f ms   (%zu entries)\n", ms,
              app.cur_tab().entries.size());
  return !app.cur_tab().entries.empty();
}

// ── timing helpers ───────────────────────────────────────────────────

struct FrameStats {
  std::vector<double> samples;
  void add(double ms) { samples.push_back(ms); }
  double min() const { return samples.empty() ? 0 : *std::min_element(samples.begin(), samples.end()); }
  double max() const { return samples.empty() ? 0 : *std::max_element(samples.begin(), samples.end()); }
  double mean() const {
    double s = 0;
    for (double v : samples) s += v;
    return samples.empty() ? 0 : s / static_cast<double>(samples.size());
  }
  double pct(double q) const {
    if (samples.empty()) return 0;
    auto v = samples;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::ceil(q * (v.size() - 1)));
    return v[idx];
  }
};

// ── main ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  int entries = 4096;
  std::string dir_path;
  int frames = 30;
  int warmup = 5;
  int scroll_steps = 0;
  bool synthetic_dir = true;
  std::vector<std::pair<int, int>> sizes = {{1600, 900}, {1920, 1080}};
  std::vector<int> modes_int;
  bool sizes_specified = false;
  std::string dump_prefix;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      return (i + 1 < argc) ? argv[++i] : "";
    };
    if (a == "--entries") entries = std::atoi(next());
    else if (a == "--dir") { dir_path = next(); synthetic_dir = false; }
    else if (a == "--dump") dump_prefix = next();
    else if (a == "--frames") frames = std::atoi(next());
    else if (a == "--warmup") warmup = std::atoi(next());
    else if (a == "--scroll-steps") scroll_steps = std::atoi(next());
    else     if (a == "--size") {
      if (!sizes_specified) sizes.clear();
      sizes_specified = true;
      std::string mm = next();
      size_t start = 0;
      while (start <= mm.size()) {
        size_t comma = mm.find(',', start);
        std::string s = mm.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        auto x = s.find('x');
        if (x != std::string::npos)
          sizes.push_back({std::atoi(s.substr(0, x).c_str()),
                           std::atoi(s.substr(x + 1).c_str())});
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    else if (a == "--mode") {
      std::string mm = next();
      size_t start = 0;
      while (start <= mm.size()) {
        size_t comma = mm.find(',', start);
        std::string m = mm.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        int v = -1;
        if (m == "list") v = 0;
        else if (m == "grid") v = 1;
        else if (m == "tree") v = 3;
        else if (m == "compact") v = 4;
        if (v < 0) { std::printf("unknown mode '%s'\n", m.c_str()); return 2; }
        modes_int.push_back(v);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    else {
      std::printf("unknown arg: %s\n", a.c_str());
      std::printf("usage: bench_paint [--entries N] [--dir PATH] [--frames N]\n"
                  "      [--warmup N] [--size WxH]... [--mode list|grid|tree|compact]...\n"
                  "      [--dump PREFIX]  (write one painted frame PNG per mode/size)\n"
                  "      [--scroll-steps N]  (grid: scroll-delta reuse frames)\n");
      return 2;
    }
  }

  // Enable EH_TRACE so paint()'s per-phase resize sink collects >= 1 ms phases.
  // Enable EH_TRACE so paint()'s per-phase resize sink collects >= 1 ms phases.
  ::setenv("EH_TRACE", "1", 1);

  std::printf("== Horizon File Browser benchmark ==\n");
  fb::AppState app;
  MemSnap m0 = read_mem();
  std::printf("baseline (AppState ctor)  : RSS %8.2f MiB  HWM %8.2f MiB  PSS %8.2f MiB\n",
              kb_to_mib(m0.vm_rss_kb), kb_to_mib(m0.vm_hwm_kb), kb_to_mib(m0.pss_kb));

  // Seed a tiny inline sidebar so draw_sidebar has representative content
  // (filesystem paths only — no D-Bus / UDisks2 involved).
  auto add_sb = [&](const char* label, const char* icon) {
    fb::SidebarLocation loc;
    loc.kind = fb::SidebarLocation::Kind::Other;
    loc.label = label;
    loc.path = "/tmp";
    loc.icon_name = icon;
    app.sidebar_locations.push_back(std::move(loc));
  };
  add_sb("Home", "user-home");
  add_sb("Documents", "folder-documents");
  add_sb("Downloads", "folder-download");
  add_sb("Pictures", "folder-pictures");
  add_sb("Trash", "user-trash");
  app.sidebar_expanded = true;

  auto t0 = std::chrono::steady_clock::now();
  fb::reload_settings_from_config(app);
  double set_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
  std::printf("  settings+colors load   %8.1f ms\n", set_ms);

  t0 = std::chrono::steady_clock::now();
  fb::warmup_text_rendering();
  double font_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
  std::printf("  font warmup             %8.1f ms\n", font_ms);

  if (synthetic_dir) {
    t0 = std::chrono::steady_clock::now();
    synthesize_entries(app, entries, "/bench/synthetic");
    double build_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::printf("  synthetic listing      %8.1f ms   (%d entries)\n", build_ms, entries);
    fb::prewarm_tab_icons(app);
  } else {
    if (!load_real_dir(app, dir_path)) {
      std::printf("  failed to load dir '%s'\n", dir_path.c_str());
      return 1;
    }
    fb::prewarm_tab_icons(app);
  }
  MemSnap m1 = read_mem();
  std::printf("memory after listing+icons:\n");
  print_mem("  (entries resident)", m1);

  std::vector<fb::ViewMode> modes;
  for (int v : modes_int) modes.push_back(static_cast<fb::ViewMode>(v));
  if (modes.empty()) modes = {fb::ViewMode::List, fb::ViewMode::Grid,
                              fb::ViewMode::Tree, fb::ViewMode::Compact};

  double peak_paint_ms = 0.0;

  // Per-mode aggregates for the current window: grid micro-counters (ns) and phase names.
  struct ModeProf {
    std::string mode_name;
    std::array<std::uint64_t, 6> micro_ns{0, 0, 0, 0, 0, 0};  // icon,label,stroke,header,hidden,flush
    std::map<std::string, std::array<double, 3>> phases;   // n, sum, max
  };

  const char* grid_micro_names[6] = {"grid-icons", "grid-labels", "grid-stroke",
                                     "group-header", "grid-hidden", "grid-flush"};

  for (auto [w, h] : sizes) {
    app.width = w;
    app.height = h;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surf);
    app.paint_profile = true;

    std::vector<ModeProf> mode_profs;

    for (fb::ViewMode mode : modes) {
      app.cur_tab().view_mode = mode;
      app.split_view = false;
      app.active_pane = 0;
      app.cur_tab().scroll_px = 0;
      app.content_reuse.valid = false;

      ModeProf mp;
      mp.mode_name = mode == fb::ViewMode::List ? "list"
                     : mode == fb::ViewMode::Grid ? "grid"
                     : mode == fb::ViewMode::Tree ? "tree"
                                                  : "compact";

      auto profile_counters =
          [&](std::array<std::uint64_t, 6>& dst) {
            dst[0] += app.profile_grid_icon_ns;
            dst[1] += app.profile_grid_label_ns;
            dst[2] += app.profile_grid_stroke_ns;
            dst[3] += app.profile_grid_header_ns;
            dst[4] += app.profile_grid_hidden_ns;
            dst[5] += app.profile_grid_flush_ns;
          };

      auto do_paint = [&](cairo_t* c) {
        app.resize_phase_samples.clear();
        app.resize_session_active = true;
        app.profile_grid_icon_ns = app.profile_grid_label_ns = 0;
        app.profile_grid_stroke_ns = app.profile_grid_header_ns = 0;
        app.profile_grid_hidden_ns = app.profile_grid_flush_ns = 0;
        fb::paint(app, c);
        for (auto& [name, ms] : app.resize_phase_samples) {
          auto& a = mp.phases[name];
          a[0] += 1;
          a[1] += ms;
          a[2] = std::max(a[2], ms);
        }
        profile_counters(mp.micro_ns);
      };

      for (int i = 0; i < warmup; ++i) do_paint(cr);

      FrameStats st;
      for (int i = 0; i < frames; ++i) {
        auto t = std::chrono::steady_clock::now();
        do_paint(cr);
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t).count();
        st.add(ms);
        peak_paint_ms = std::max(peak_paint_ms, ms);
      }

      if (!dump_prefix.empty()) {
        std::string p = dump_prefix + "-grid@";
        p += mp.mode_name + "-" + std::to_string(w) + "x" + std::to_string(h) + ".png";
        cairo_surface_write_to_png(cairo_get_target(cr), p.c_str());
        std::printf("  [dump] %s\n", p.c_str());
      }

      if (mode == modes[0]) {
        std::printf("\nwindow %dx%d (%.1f MP | %d frames)\n", w, h,
                    static_cast<double>(w) * h / 1e6, frames);
        std::printf("  %-8s %8s %8s %8s %8s %8s %9s\n", "mode", "avg", "min", "p50",
                    "p95", "max", "fps(avg)");
      }
      double avg = st.mean();
      std::printf("  %-8s %7.2f %7.2f %7.2f %7.2f %7.2f  %8.1f\n",
                  mp.mode_name.c_str(), avg, st.min(), st.pct(0.50), st.pct(0.95),
                  st.max(), avg > 0 ? 1000.0 / avg : 0.0);

      // Scroll-delta reuse pass (grid only): advance scroll by one row per
      // frame through fb::make_content_reuse_hint so the real reuse path —
      // in-place shift + exposed-band strip draw — is what gets timed.
      // paint() skips its own scroll advance when a hint is passed, so we
      // drive scroll_smooth_target/current directly and jump first.
      if (scroll_steps > 0 && mode == fb::ViewMode::Grid) {
        // Seed the reuse record from the battery's last full frame (scroll 0)
        // so even a 1-step run reuses on its first timed frame rather than
        // silently timing a full repaint as "reuse".
        fb::record_content_reuse(app, 0);
        const int step = std::max(8, app.grid_row_h);
        const int first_scroll = step;
        app.cur_tab().scroll_smooth_target = first_scroll;
        app.cur_tab().scroll_smooth_current = first_scroll;
        fb::advance_scroll_render(app);  // scroll_px = first_scroll
        int reuse_frames = 0, reuse_ok = 0;
        double rt = 0, rmin = 1e9, rmax = 0;
        for (int i = 0; i < scroll_steps; ++i) {
          if (i > 0) {
            app.cur_tab().scroll_smooth_target += step;
            app.cur_tab().scroll_smooth_current = app.cur_tab().scroll_smooth_target;
            fb::advance_scroll_render(app);
          }
          fb::ContentReuseHint h = fb::make_content_reuse_hint(app);
          auto t0 = std::chrono::steady_clock::now();
          fb::paint(app, cr, &h);
          double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
          fb::record_content_reuse(app, 0);
          ++reuse_frames;
          if (h.delta) ++reuse_ok;
          rt += ms;
          rmin = std::min(rmin, ms);
          rmax = std::max(rmax, ms);
        }
        double ravg = rt / static_cast<double>(reuse_frames);
        std::printf("  %-8s reuse %7.2f %7.2f %7.2f %7.2f %7.2f  %8.1f  (%d/%d frames, end scroll %d, step %d)\n",
                    mp.mode_name.c_str(), ravg, rmin,
                    ravg, rmax, rmax, ravg > 0 ? 1000.0 / ravg : 0.0,
                    reuse_ok, reuse_frames, app.cur_tab().scroll_px, step);
        if (!dump_prefix.empty()) {
          // Byte-level identity probe vs a second surface. Surface A (current)
          // paints pre, then the ONE reuse frame. Surface B paints a fresh
          // full frame at the target scroll. Content rows must match.
          const int end_scroll = app.cur_tab().scroll_px;
          const int pre_scroll = end_scroll - step;
          auto raw = [](cairo_surface_t* s, int& stride) -> const uint8_t* {
            cairo_surface_flush(s);
            stride = cairo_image_surface_get_stride(s);
            return reinterpret_cast<const uint8_t*>(cairo_image_surface_get_data(s));
          };
          app.cur_tab().scroll_smooth_target = pre_scroll;
          app.cur_tab().scroll_smooth_current = pre_scroll;
          fb::advance_scroll_render(app);
          // Mirror draw(): clear first so this full frame is composited over
          // transparency like surface B — otherwise the 0.53 surf_alpha bg
          // fill stacks over stale frame history and every row diverges.
          cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
          cairo_paint(cr);
          cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
          fb::paint(app, cr);
          fb::record_content_reuse(app, 0);
          std::printf("  [identity] pre paint: scroll_px=%d target=%.0f cur=%.0f grid_row_h=%d group_field=%d sort=%d vis=%d\n",
                      app.cur_tab().scroll_px, app.cur_tab().scroll_smooth_target,
                      app.cur_tab().scroll_smooth_current, app.grid_row_h,
                      app.cur_tab().group_field, static_cast<int>(app.cur_tab().sort_field),
                      (int)app.cur_tab().visible_entries.size());
          std::string pre_p = dump_prefix + "-pre.png";
          cairo_surface_write_to_png(cairo_get_target(cr), pre_p.c_str());
          // Snapshot pre content rows (control reference) before shifting.
          int cx2, cy2, cw2, ch2, bh2;
          fb::content_reuse_geometry(app, cx2, cy2, cw2, ch2, bh2);
          cairo_surface_flush(cairo_get_target(cr));
          int strPre = 0;
          const uint8_t* P = reinterpret_cast<const uint8_t*>(
              cairo_image_surface_get_data(cairo_get_target(cr)));
          std::vector<uint8_t> preBUF((size_t)ch2 * (size_t)cw2 * 4u);
          for (int r = 0; r < ch2; ++r)
            std::memcpy(preBUF.data() + (size_t)r * (size_t)cw2 * 4u,
                        P + (size_t)(cy2 + r) * strPre + (size_t)cx2 * 4u,
                        (size_t)cw2 * 4u);
          app.cur_tab().scroll_smooth_target = end_scroll;
          app.cur_tab().scroll_smooth_current = end_scroll;
          fb::advance_scroll_render(app);
          fb::ContentReuseHint rh = fb::make_content_reuse_hint(app);
          std::printf("  [identity] reuse: scroll_px=%d step=%d hint.delta=%d\n",
                      app.cur_tab().scroll_px, step, rh.delta);
          fb::paint(app, cr, &rh);
          std::string reuse_p = dump_prefix + "-reuse.png";
          cairo_surface_write_to_png(cairo_get_target(cr), reuse_p.c_str());
          // Surface B: full repaint at target scroll.
          cairo_surface_t* sB = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
          cairo_t* crB = cairo_create(sB);
          fb::paint(app, crB);
          cairo_destroy(crB);
          std::string full_p = dump_prefix + "-full.png";
          cairo_surface_write_to_png(sB, full_p.c_str());
          int strA = 0, strB = 0;
          const uint8_t* A = raw(cairo_get_target(cr), strA);
          const uint8_t* B = raw(sB, strB);
          int first_bad = -1, bad_rows = 0;
          for (int r = 0; r < ch2; ++r) {
            const uint8_t* ra = A + (cy2 + r) * strA + cx2 * 4;
            const uint8_t* rb = B + (cy2 + r) * strB + cx2 * 4;
            if (std::memcmp(ra, rb, static_cast<size_t>(cw2) * 4) != 0) {
              if (first_bad < 0) first_bad = r;
              ++bad_rows;
            }
          }
          std::printf("  [identity] reuse-vs-full differing rows: %d/%d (end_scroll %d, step %d)\n",
                      bad_rows, ch2, end_scroll, step);
          // Determinism control: a second full paint at the same scroll after
          // a pause must reproduce surface B byte-for-byte (catches time-based
          // spinners, thumbnails, etc.).
          cairo_surface_t* sC = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
          cairo_t* crC = cairo_create(sC);
          usleep(300000);
          fb::paint(app, crC);
          cairo_destroy(crC);
          int strC = 0;
          const uint8_t* C = raw(sC, strC);
          int ndet = 0;
          for (int r = 0; r < ch2; ++r) {
            const uint8_t* rb = B + (cy2 + r) * strB + cx2 * 4;
            const uint8_t* rc = C + (cy2 + r) * strC + cx2 * 4;
            if (std::memcmp(rb, rc, static_cast<size_t>(cw2) * 4) != 0)
              ++ndet;
          }
          std::printf("  [identity] full@%d-vs-full@%d differing rows: %d/%d (determinism control)\n",
                      end_scroll, end_scroll, ndet, ch2);
          // Oracle: reuse must not introduce ANY row difference a full paint
          // would not. Each output row where reuse != full must also differ
          // between full and the pre-shift source (a known full-paint
          // divergence, e.g. icon/label AA at a scrolled offset). Retained
          // rows (r < ch2-delta for delta>0, else r >= -delta) map to pre
          // row r+delta; band rows have no shift source, so reuse must equal
          // full there outright.
          int reuse_bad = 0, oracle_bad = 0;
          for (int r = 0; r < ch2; ++r) {
            const uint8_t* ra = A + (cy2 + r) * strA + cx2 * 4;
            const uint8_t* rb = B + (cy2 + r) * strB + cx2 * 4;
            if (std::memcmp(ra, rb, static_cast<size_t>(cw2) * 4) == 0) continue;
            ++reuse_bad;
            int src = r + step;  // delta for this identity run is exactly step
            const bool retained =
                (step > 0 && r < ch2 - step) || (step < 0 && r >= -step);
            bool full_diverges = false;
            if (retained && src >= 0 && src < ch2) {
              const uint8_t* ps = preBUF.data() + (size_t)src * (size_t)cw2 * 4u;
              full_diverges = std::memcmp(rb, ps, static_cast<size_t>(cw2) * 4) != 0;
            } else {
              full_diverges = true;  // band row: reuse must match full, so any
                                     // difference here is a real reuse defect
            }
            if (!full_diverges) ++oracle_bad;
          }
          std::printf("  [identity] reuse oracle: %d rows differ from full, %d NOT explained by full-paint divergence (pass=%s)\n",
                      reuse_bad, oracle_bad, oracle_bad == 0 ? "PASS" : "FAIL");
          // Up-scroll pass: verify the negative-delta path (band at the TOP).
          // The surface currently holds the end_scroll frame (== full there);
          // reuse it as the pre for scrolling back up one step.
          {
            const int up_target = end_scroll > 0 ? end_scroll - step : end_scroll;
            std::vector<uint8_t> preUP((size_t)ch2 * (size_t)cw2 * 4u);
            for (int r = 0; r < ch2; ++r)
              std::memcpy(preUP.data() + (size_t)r * (size_t)cw2 * 4u,
                          A + (size_t)(cy2 + r) * strA + (size_t)cx2 * 4u,
                          (size_t)cw2 * 4u);
            fb::record_content_reuse(app, 0);
            app.cur_tab().scroll_smooth_target = up_target;
            app.cur_tab().scroll_smooth_current = up_target;
            fb::advance_scroll_render(app);
            fb::ContentReuseHint rhu = fb::make_content_reuse_hint(app);
            fb::paint(app, cr, &rhu);
            cairo_surface_t* sB2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t* crB2 = cairo_create(sB2);
            fb::paint(app, crB2);
            cairo_destroy(crB2);
            int strA2 = 0, strB2 = 0;
            const uint8_t* A2 = raw(cairo_get_target(cr), strA2);
            const uint8_t* B2 = raw(sB2, strB2);
            int ubad = 0, uoracle = 0;
            const int ust = up_target - end_scroll;  // -step (or 0)
            for (int r = 0; r < ch2; ++r) {
              const uint8_t* ra = A2 + (cy2 + r) * strA2 + cx2 * 4;
              const uint8_t* rb = B2 + (cy2 + r) * strB2 + cx2 * 4;
              if (std::memcmp(ra, rb, static_cast<size_t>(cw2) * 4) == 0) continue;
              ++ubad;
              int src = r + ust;
              const bool retained = (ust < 0 && r >= -ust);
              bool fd = false;
              if (retained && src >= 0 && src < ch2)
                fd = std::memcmp(rb, preUP.data() + (size_t)src * (size_t)cw2 * 4u,
                                 static_cast<size_t>(cw2) * 4) != 0;
              else
                fd = true;
              if (!fd) ++uoracle;
            }
            std::printf("  [identity] reuse-up(hint.delta=%d) vs full-up: %d rows differ, %d NOT explained (pass=%s)\n",
                        rhu.delta, ubad, uoracle, uoracle == 0 ? "PASS" : "FAIL");
            cairo_surface_destroy(sB2);
          }
          // Cross-match: does pre content appear anywhere in full? Hash map by
          // row hash + memcmp verification, dy histogram.
          {
            auto rhash = [](const uint8_t* p, int bytes) {
              uint64_t x = 1469598103934665603ull;
              for (int i = 0; i < bytes; i += 7) { x ^= p[i]; x *= 1099511628211ull; }
              return x;
            };
            int cb = cw2 * 4;
            std::unordered_multimap<uint64_t, int> ph;
            for (int r = 0; r < ch2; ++r)
              ph.emplace(rhash(preBUF.data() + (size_t)r * (size_t)cb, cb), r);
            std::vector<int> dyh(2 * ch2 + 1, 0);
            int matched = 0, verified = 0;
            for (int r = 0; r < ch2; ++r) {
              uint64_t hr = rhash(B + (size_t)(cy2 + r) * strB + (size_t)cx2 * 4u, cb);
              auto range = ph.equal_range(hr);
              for (auto it = range.first; it != range.second; ++it) {
                ++matched;
                const uint8_t* pa = preBUF.data() + (size_t)it->second * (size_t)cb;
                const uint8_t* pb = B + (size_t)(cy2 + r) * strB + (size_t)cx2 * 4u;
                if (std::memcmp(pa, pb, (size_t)cb) == 0) {
                  ++verified;
                  ++dyh[it->second - r + ch2];
                }
              }
            }
            int modald = 0, modan = -1;
            for (int d = 0; d < 2 * ch2 + 1; ++d)
              if (dyh[d] > modan) { modan = dyh[d]; modald = d - ch2; }
            std::printf("  [identity] pre rows present in full: %d/%d (verified %d), modal dy=%d\n",
                        matched, ch2, verified, modald);
          }
          cairo_surface_destroy(sC);
          // Row-alignment search between two full paints (control) and
          // between reuse and full.
          auto align = [&](const std::vector<uint8_t>& X,
                           const uint8_t* Y, int ystride, const char* tag) {
            auto rhash = [](const uint8_t* p, int bytes) {
              uint64_t x = 1469598103934665603ull;
              for (int i = 0; i < bytes; i += 7) { x ^= p[i]; x *= 1099511628211ull; }
              return x;
            };
            int content_bytes = cw2 * 4;
            std::vector<uint64_t> hX(ch2), hY(ch2);
            for (int r = 0; r < ch2; ++r) {
              hX[r] = rhash(X.data() + (size_t)r * (size_t)content_bytes, content_bytes);
              hY[r] = rhash(Y + (size_t)(cy2 + r) * ystride + (size_t)cx2 * 4u, content_bytes);
            }
            int best_dy = 0, best_n = -1;
            int skip_top = std::min(6, ch2);
            for (int dy = -(ch2 - skip_top); dy <= ch2 - skip_top; ++dy) {
              int n = 0;
              for (int r = skip_top; r < ch2; ++r) {
                int rr = r + dy;
                if (rr < 0 || rr >= ch2) continue;
                n += hX[r] == hY[rr];
              }
              if (n > best_n) { best_n = n; best_dy = dy; }
            }
            std::printf("  [identity] %-18s best dy=%d (%d/%d rows)\n",
                        tag, best_dy, best_n, ch2 - skip_top);
            return best_dy;
          };
          int dy_ctl = align(preBUF, B, strB, "pre-vs-full");
          int dy_re = align(preBUF, A, strA, "pre-vs-reuse");
          // Align reuse directly against pre and full.
          (void)dy_ctl; (void)dy_re;
          cairo_surface_destroy(sB);
          std::printf("  [dump] %s / %s vs %s\n", pre_p.c_str(), reuse_p.c_str(), full_p.c_str());
        }
      }

      mode_profs.push_back(std::move(mp));
    }

    // Ranked per-mode breakdown for this window: biggest offenders first.
    std::printf("\n  biggest offenders (%.1f MP, %d frames per mode):\n",
                static_cast<double>(w) * h / 1e6, frames);
    for (auto& mp : mode_profs) {
      std::vector<std::pair<std::string, double>> rows;  // label, avg ms
      for (int i = 0; i < 6; ++i) {
        double avg_ms = static_cast<double>(mp.micro_ns[i]) / 1e6 / frames;
        if (avg_ms >= 0.001) rows.emplace_back(grid_micro_names[i], avg_ms);
      }
      for (auto& [name, a] : mp.phases) {
        double avg_ms = a[1] / frames;  // sum / shared frame divisor
        if (avg_ms >= 0.001) rows.emplace_back(name, avg_ms);
      }
      std::sort(rows.begin(), rows.end(),
                [](const auto& l, const auto& r) { return l.second > r.second; });
      if (rows.empty()) {
        std::printf("    %s: (all phases under 0.001 ms avg)\n", mp.mode_name.c_str());
        continue;
      }
      std::printf("    %s:\n", mp.mode_name.c_str());
      for (auto& [label, ms] : rows)
        std::printf("      %-16s %10.3f ms\n", label.c_str(), ms);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    app.paint_profile = false;
  }

  MemSnap m2 = read_mem();
  std::printf("\nfinal memory:\n");
  print_mem("  (after all paints)", m2);
  std::printf("\npeak single-frame paint: %.2f ms\n", peak_paint_ms);

  fb::join_scan(app);
  return 0;
}