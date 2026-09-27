// Consistency checks for the stage script (no test framework needed).
#include <cmath>
#include <cstdio>
#include <string>

#include "src/scene/boot_script.h"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace bootviz::scene;
  const auto& stages = BootStages();
  Check(stages.size() == 8, "eight stages (keys 1-8)");
  float total = 0;
  long long prev_cycle = 0;
  for (size_t i = 0; i < stages.size(); ++i) {
    const Stage& s = stages[i];
    const std::string id = "stage " + std::to_string(i + 1) + " (" + s.name + ")";
    Check(s.duration > 0, id + ": positive duration");
    Check(!s.steps.empty() && !s.title.empty() && !s.actor.empty() && !s.where.empty(),
          id + ": has title, actor, source and steps");
    Check(s.showcase >= 0 && s.showcase <= 1, id + ": showcase in [0,1]");
    Check(!s.log.empty(), id + ": has trace lines");
    Check(!s.active.empty(), id + ": lights some blocks");
    Check(s.cycle_from <= s.cycle_to && s.cycle_from >= prev_cycle,
          id + ": bytecode range is ordered");
    prev_cycle = s.cycle_to;
    float prev = -1;
    for (const LogLine& l : s.log) {
      Check(l.at >= prev && l.at >= 0 && l.at <= 1, id + ": log times ascend within [0,1]");
      prev = l.at;
    }
    for (const Flow& f : s.flows) {
      Check(f.from != f.to && f.from < Part::kCount && f.to < Part::kCount, id + ": flow ends");
      Check(f.start >= 0 && f.start <= f.end && f.end <= 1, id + ": flow window");
      Check(f.label_at >= 0 && f.label_at <= 1, id + ": label position");
    }
    for (const Damage& d : s.damage) {
      Check(d.x >= 0 && d.y >= 0 && d.x + d.w <= 1024 && d.y + d.h <= 1024,
            id + ": damage inside the 1024x1024 display");
    }
    total += s.duration;
  }
  Check(std::fabs(total - TotalDuration()) < 1e-4f, "TotalDuration sums the stages");

  // Cursor <-> seconds round trip, and clamping at both ends.
  for (float t = 0; t < TotalDuration(); t += 0.37f) {
    const Cursor c = CursorAt(t);
    Check(std::fabs(SecondsAt(c) - t) < 1e-3f, "round trip at t=" + std::to_string(t));
  }
  Check(CursorAt(-5).stage == 0 && CursorAt(-5).progress == 0, "clamps before the start");
  const Cursor end = CursorAt(TotalDuration() + 10);
  Check(end.stage == static_cast<int>(stages.size()) - 1 && end.progress == 1, "clamps at the end");

  // The trace only grows; the display appears with beDisplay (stage 5) and
  // the shown screen never goes back in time.
  size_t prev_lines = 0;
  long long prev_screen = -1;
  for (float t = 0; t <= TotalDuration(); t += 0.25f) {
    const Cursor c = CursorAt(t);
    const size_t n = TraceUpTo(c).size();
    Check(n >= prev_lines, "trace never shrinks");
    prev_lines = n;
    const Screen* s = ScreenAt(c);
    if (c.stage < 4) Check(s == nullptr, "no display before beDisplay");
    if (s) {
      Check(s->cycle >= prev_screen, "screens advance");
      prev_screen = s->cycle;
    }
  }
  Check(ScreenAt({static_cast<int>(stages.size()) - 1, 1.f}) != nullptr, "a screen at the end");
  const auto all = TraceUpTo({static_cast<int>(stages.size()) - 1, 1.f});
  bool be_display = false;
  for (const std::string& l : all) be_display |= l.find("set_display_size(1024, 1024)") != std::string::npos;
  Check(be_display, "the trace shows set_display_size(1024, 1024)");

  // File regions tile the snapshot file.
  uint64_t covered = 0;
  for (const FileRegion& r : ImageFileMap()) {
    Check(r.size > 0 && r.base + r.size <= kImageFileSize, r.label + ": inside the file");
    covered += r.size;
  }
  Check(covered == kImageFileSize, "file regions cover standardImage exactly");

  if (failures) return 1;
  std::printf("boot_script_test: all checks passed\n");
  return 0;
}
