#include "src/scene/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "include/core/SkBlurTypes.h"
#include "include/core/SkColor.h"
#include "include/core/SkContourMeasure.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "src/scene/screens.h"

namespace bootviz::scene {
namespace {

// Palette (ARGB).
constexpr SkColor kPanel = 0xD90B1620;
constexpr SkColor kPanelBorder = 0xFF1F3444;
constexpr SkColor kText = 0xFFE6EEF5;
constexpr SkColor kMuted = 0xFF8FA3B5;
constexpr SkColor kFaint = 0xFF4E6272;
constexpr SkColor kBlock = 0xE6122230;
constexpr SkColor kBlockDim = 0xB30E1A24;
constexpr SkColor kTrace = 0xFF22394A;
constexpr SkColor kInk = 0xFF0B1620;

// Lane colours: the image, the VM, the host.
constexpr SkColor kLaneColor[kLaneCount] = {0xFF6BE38A, 0xFF5CD6FF, 0xFFFFB547};
constexpr const char* kLaneTitle[kLaneCount] = {
    "SMALLTALK-80 IMAGE   objects whose methods run as bytecodes",
    "C++ VIRTUAL MACHINE   src/interpreter.cpp, objmemory.cpp, bitblt.cpp",
    "HOST   main_sdl.cpp, Linux, SDL2",
};

SkColor WithAlpha(SkColor c, float a) {
  return SkColorSetA(c, static_cast<U8CPU>(std::clamp(a, 0.f, 1.f) * 255.f));
}

SkColor Mix(SkColor a, SkColor b, float t) {
  t = std::clamp(t, 0.f, 1.f);
  auto ch = [t](unsigned x, unsigned y) {
    return static_cast<U8CPU>(std::lround(x + (static_cast<float>(y) - x) * t));
  };
  return SkColorSetARGB(ch(SkColorGetA(a), SkColorGetA(b)), ch(SkColorGetR(a), SkColorGetR(b)),
                        ch(SkColorGetG(a), SkColorGetG(b)), ch(SkColorGetB(a), SkColorGetB(b)));
}

SkPaint Fill(SkColor c) {
  SkPaint p;
  p.setAntiAlias(true);
  p.setColor(c);
  return p;
}

SkPaint Stroke(SkColor c, float width) {
  SkPaint p = Fill(c);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(width);
  return p;
}

void Panel(SkCanvas* c, SkRect r, float radius = 12.f) {
  c->drawRRect(SkRRect::MakeRectXY(r, radius, radius), Fill(kPanel));
  c->drawRRect(SkRRect::MakeRectXY(r.makeInset(0.5f, 0.5f), radius, radius),
               Stroke(kPanelBorder, 1.f));
}

void Glow(SkCanvas* c, SkRect r, float radius, SkColor color, float sigma) {
  SkPaint p = Fill(color);
  p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
  c->drawRRect(SkRRect::MakeRectXY(r, radius, radius), p);
}

float Smooth(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

std::string Grouped(long long v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
  return s;
}

// ---------------------------------------------------------------------------
// Diagram layout (design coordinates, 1600x900)
// ---------------------------------------------------------------------------

const SkRect kDiagram = SkRect::MakeLTRB(24, 140, 1004, 644);
constexpr float kLaneLeft = 40, kLaneRight = 988;
constexpr float kLaneTop0 = 180, kLaneStep = 106, kLaneHeight = 98;
constexpr float kBlockX0 = 56, kBlockStep = 155, kBlockW = 140;
constexpr float kBlockTop = 26, kBlockH = 58;  // within the lane

SkRect LaneRect(int lane) {
  const float top = kLaneTop0 + lane * kLaneStep;
  return SkRect::MakeLTRB(kLaneLeft, top, kLaneRight, top + kLaneHeight);
}

struct Block {
  const char* label;
  const char* sub;
};

const Block& BlockOf(Part p) {
  static const Block blocks[static_cast<int>(Part::kCount)] = {
      {"Processor", "active Process"},
      {"Controllers", "ScheduledControllers"},
      {"BitBlt", "DisplayScanner"},
      {"Display", "DisplayScreen form"},
      {"Sensor", "InputState process"},
      {"TekSystemCall", "UniFLEX calls"},
      {"ObjectMemory", "object table + heap"},
      {"Interpreter", "cycle(): bytecodes"},
      {"Message send", "lookup, contexts"},
      {"Primitives", "dispatchPrimitives()"},
      {"BitBlt engine", "src/bitblt.cpp"},
      {"Semaphores", "asynchronousSignal()"},
      {"standardImage", "snapshot file"},
      {"IFileSystem", "PosixST80FileSystem"},
      {"main()", "event loop"},
      {"SDLHAL", "the HAL, hal.h"},
      {"SDL2", "window, texture"},
      {"Mouse, keyboard", "SDL events"},
  };
  return blocks[static_cast<int>(p)];
}

SkRect BlockRect(Part p) {
  const SkRect lane = LaneRect(LaneOf(p));
  const float x = kBlockX0 + SlotOf(p) * kBlockStep;
  return SkRect::MakeXYWH(x, lane.fTop + kBlockTop, kBlockW, kBlockH);
}

// Orthogonal route between two blocks.
//  - Same lane, neighbours: straight across (rightward runs sit a little
//    higher than leftward ones so the two directions do not overlap).
//  - Same lane, farther apart: down under the lane's blocks and back up.
//  - Different lanes: out of the facing edge, along the gap above the lower
//    lane, into the other block (downward runs left of centre, upward right).
SkPath Route(Part a, Part b) {
  const SkRect ra = BlockRect(a), rb = BlockRect(b);
  const int la = LaneOf(a), lb = LaneOf(b);
  SkPathBuilder pb;
  if (la == lb) {
    const bool right = SlotOf(b) > SlotOf(a);
    if (std::abs(SlotOf(a) - SlotOf(b)) == 1) {
      const float y = ra.centerY() + (right ? -8.f : 8.f);
      pb.moveTo(right ? ra.fRight : ra.fLeft, y);
      pb.lineTo(right ? rb.fLeft : rb.fRight, y);
    } else {
      const float y = ra.fBottom + (right ? 6.f : 10.f);
      const float xa = ra.centerX() + (right ? 30.f : -30.f);
      const float xb = rb.centerX() + (right ? -30.f : 30.f);
      pb.moveTo(xa, ra.fBottom);
      pb.lineTo(xa, y);
      pb.lineTo(xb, y);
      pb.lineTo(xb, rb.fBottom);
    }
    return pb.detach();
  }
  const bool down = lb > la;
  const float off = down ? -9.f : 9.f;
  // Pull the ends toward each other so diagonal neighbours meet near the gap.
  const float xa = std::clamp(rb.centerX(), ra.fLeft + 18, ra.fRight - 18) + off;
  const float xb = std::clamp(ra.centerX(), rb.fLeft + 18, rb.fRight - 18) + off;
  const float gap = LaneRect(std::max(la, lb)).fTop - 4.f + (down ? -2.f : 2.f);
  const float ya = down ? ra.fBottom : ra.fTop;
  const float yb = down ? rb.fTop : rb.fBottom;
  pb.moveTo(xa, ya);
  if (std::fabs(xa - xb) < 0.5f) {
    pb.lineTo(xb, yb);
  } else {
    pb.lineTo(xa, gap);
    pb.lineTo(xb, gap);
    pb.lineTo(xb, yb);
  }
  return pb.detach();
}

SkColor TraceColor(const std::string& line) {
  if (line.rfind("#", 0) == 0) return 0xFF6F8798;
  if (line.rfind("$", 0) == 0) return 0xFFFFFFFF;
  if (line.size() < 22) return 0xFFD7E3EC;
  const std::string kind = line.substr(17, 4);
  if (kind == "prim") return 0xFF8FDCF5;
  if (kind == "hal ") return 0xFF9BEBB0;
  if (kind == "tek ") return 0xFFFFC870;
  if (kind == "fs  ") return 0xFFC9B2FF;
  if (kind == "host") return 0xFFF2F6F9;
  if (kind == "vm  ") return 0xFFD4EE8E;
  return 0xFFD7E3EC;
}

}  // namespace

// ---------------------------------------------------------------------------

void Scene::Render(const FrameModel& m, uint8_t* pixels) {
  const SkImageInfo info = SkImageInfo::MakeN32Premul(m.width, m.height);
  std::unique_ptr<SkCanvas> canvas =
      SkCanvas::MakeRasterDirect(info, pixels, static_cast<size_t>(m.width) * 4);
  SkCanvas* c = canvas.get();
  c->clear(SK_ColorTRANSPARENT);
  const float s = std::min(m.width / 1600.f, m.height / 900.f);
  c->translate((m.width - 1600.f * s) * 0.5f, (m.height - 900.f * s) * 0.5f);
  c->scale(s, s);
  DrawHeader(c, m);
  DrawRail(c, m);
  DrawDiagram(c, m);
  DrawFlows(c, m);
  DrawSnapshotBand(c, m);
  DrawInfo(c, m);
  DrawScreen(c, m);
  DrawTrace(c, m);
}

void Scene::DrawHeader(SkCanvas* c, const FrameModel& m) {
  const Stage& st = BootStages()[m.cursor.stage];
  c->drawRect(SkRect::MakeLTRB(-400, -400, 2000, 132), Fill(0x99060D13));
  c->drawLine(-400, 132, 2000, 132, Stroke(0x401F3444, 1.f));
  // Logo mark: a small Tektronix-style screen with a Smalltalk balloon.
  c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(28, 18, 30, 30), 6, 6), Fill(st.accent));
  c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(34, 24, 18, 14), 2, 2), Fill(kInk));
  c->drawRect(SkRect::MakeXYWH(38, 40, 10, 3), Fill(kInk));
  const float tw = DrawText(c, "How Tektronix 4404 Smalltalk-80 boots on this VM", 72, 42,
                            fonts_->Bold(26), kText);
  DrawText(c, "and how its calls reach the host: primitives, HAL, SDL", 72 + tw + 16, 42,
           fonts_->Regular(15), kMuted);

  char right[96];
  std::snprintf(right, sizeof right, "t = %5.1f s / %.0f s   %s %.1fx", SecondsAt(m.cursor),
                TotalDuration(), m.paused ? "paused" : "playing", m.speed);
  DrawText(c, right, 1572, 32, fonts_->Mono(14), kMuted, Align::kRight);
  if (m.show_keys) {
    DrawText(c, "Space pause · ←/→ stage · 1-8 jump · +/− speed · R restart", 1572, 52,
             fonts_->Regular(12.5f), kFaint, Align::kRight);
  }
}

void Scene::DrawRail(SkCanvas* c, const FrameModel& m) {
  const auto& stages = BootStages();
  const int n = static_cast<int>(stages.size());
  const float x0 = 60, x1 = 1540, y = 92;
  const float step = (x1 - x0) / (n - 1);
  c->drawLine(x0, y, x1, y, Stroke(kTrace, 4));
  const float done_x = x0 + step * (m.cursor.stage + m.cursor.progress);
  SkPaint prog = Stroke(stages[m.cursor.stage].accent, 4);
  prog.setStrokeCap(SkPaint::kRound_Cap);
  c->drawLine(x0, y, std::min(done_x, x1), y, prog);
  for (int i = 0; i < n; ++i) {
    const float x = x0 + step * i;
    const bool current = i == m.cursor.stage;
    const bool done = i < m.cursor.stage;
    const SkColor col = current || done ? stages[i].accent : kFaint;
    if (current) {
      const float pulse = 0.5f + 0.5f * std::sin(m.time * 4.f);
      c->drawCircle(x, y, 17 + 3 * pulse, Fill(WithAlpha(col, 0.18f)));
    }
    c->drawCircle(x, y, 11, Fill(done ? col : kInk));
    c->drawCircle(x, y, 11, Stroke(col, 2.5f));
    char num[12];
    std::snprintf(num, sizeof num, "%d", i + 1);
    DrawText(c, num, x, y + 4.5f, fonts_->Bold(12), done ? kInk : col, Align::kCenter);
    DrawText(c, stages[i].name, x, y + 34, current ? fonts_->Bold(14) : fonts_->Regular(13.5f),
             current ? kText : (done ? kMuted : kFaint), Align::kCenter);
  }
}

void Scene::DrawDiagram(SkCanvas* c, const FrameModel& m) {
  const Stage& st = BootStages()[m.cursor.stage];
  Panel(c, kDiagram);
  const float tw = DrawText(c, "Image, virtual machine, host", 44, 166, fonts_->Bold(16), kText);
  DrawText(c, "lit blocks take part in this stage; moving dots are calls and data", 44 + tw + 12,
           166, fonts_->Regular(12.5f), kFaint);

  // Lanes.
  for (int lane = 0; lane < kLaneCount; ++lane) {
    const SkRect r = LaneRect(lane);
    c->drawRRect(SkRRect::MakeRectXY(r, 9, 9), Fill(WithAlpha(kLaneColor[lane], 0.045f)));
    c->drawRRect(SkRRect::MakeRectXY(r.makeInset(0.5f, 0.5f), 9, 9),
                 Stroke(WithAlpha(kLaneColor[lane], 0.28f), 1.f));
    c->drawRect(SkRect::MakeXYWH(r.fLeft + 12, r.fTop + 9, 3, 11),
                Fill(WithAlpha(kLaneColor[lane], 0.9f)));
    DrawText(c, kLaneTitle[lane], r.fLeft + 22, r.fTop + 19, fonts_->Bold(10.5f),
             WithAlpha(kLaneColor[lane], 0.85f));
  }

  // Blocks.
  for (int i = 0; i < static_cast<int>(Part::kCount); ++i) {
    const Part part = static_cast<Part>(i);
    const Block& b = BlockOf(part);
    const SkRect r = BlockRect(part);
    const bool active = std::find(st.active.begin(), st.active.end(), part) != st.active.end();
    const SkColor border = active ? st.accent : 0xFF2A3E4E;
    if (active) Glow(c, r.makeOutset(2, 2), 8, WithAlpha(border, 0.42f), 7);
    c->drawRRect(SkRRect::MakeRectXY(r, 7, 7), Fill(active ? kBlock : kBlockDim));
    c->drawRRect(SkRRect::MakeRectXY(r.makeInset(0.75f, 0.75f), 7, 7),
                 Stroke(border, active ? 2.f : 1.2f));
    // Lane tick on the left edge.
    c->drawRect(SkRect::MakeXYWH(r.fLeft + 1.5f, r.fTop + 12, 2.5f, r.height() - 24),
                Fill(WithAlpha(kLaneColor[LaneOf(part)], active ? 0.9f : 0.35f)));
    const SkFont label_font = fonts_->Bold(12.5f);
    const std::string label = Ellipsize(label_font, b.label, r.width() - 14);
    DrawText(c, label, r.centerX(), r.fTop + 26, label_font, active ? kText : kMuted,
             Align::kCenter);
    DrawText(c, Ellipsize(fonts_->Regular(10.5f), b.sub, r.width() - 12), r.centerX(),
             r.fTop + 43, fonts_->Regular(10.5f), active ? kMuted : kFaint, Align::kCenter);
  }
}

void Scene::DrawFlows(SkCanvas* c, const FrameModel& m) {
  const Stage& st = BootStages()[m.cursor.stage];
  const float p = m.cursor.progress;
  struct Label {
    SkRect pill;
    std::string text;
    SkColor color;
  };
  std::vector<Label> labels;
  for (const Flow& f : st.flows) {
    if (p < f.start) continue;
    const float local = f.end > f.start ? (p - f.start) / (f.end - f.start) : 1.f;
    const bool running = local <= 1.f;
    const SkPath path = Route(f.from, f.to);
    SkContourMeasureIter iter(path, false);
    sk_sp<SkContourMeasure> cm = iter.next();
    if (!cm) continue;
    const float len = cm->length();
    const float fade = running ? 1.f : 0.3f;
    SkPaint lit = Stroke(WithAlpha(f.color, 0.6f * fade), 2.5f);
    lit.setStrokeCap(SkPaint::kRound_Cap);
    if (running) {
      const float head = len * Smooth(std::min(1.f, local * 3.f));
      SkPathBuilder partial;
      if (cm->getSegment(0, head, &partial, true)) c->drawPath(partial.detach(), lit);
      const float spacing = 22.f;
      const float offset = std::fmod(m.time * 80.f, spacing);
      for (float d = offset; d < head; d += spacing) {
        SkPoint pos;
        SkVector tan;
        if (!cm->getPosTan(d, &pos, &tan)) continue;
        SkPaint dot = Fill(f.color);
        dot.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1.2f));
        c->drawCircle(pos.fX, pos.fY, 3.4f, dot);
      }
      // Arrow head at the destination once the trace arrives.
      if (head >= len - 0.5f) {
        SkPoint pos;
        SkVector tan;
        if (cm->getPosTan(len, &pos, &tan)) {
          SkPathBuilder arrow;
          const SkVector n{-tan.fY, tan.fX};
          arrow.moveTo(pos);
          arrow.lineTo(pos.fX - tan.fX * 8 + n.fX * 4.5f, pos.fY - tan.fY * 8 + n.fY * 4.5f);
          arrow.lineTo(pos.fX - tan.fX * 8 - n.fX * 4.5f, pos.fY - tan.fY * 8 - n.fY * 4.5f);
          arrow.close();
          c->drawPath(arrow.detach(), Fill(f.color));
        }
      }
    } else {
      c->drawPath(path, lit);
    }
    if (!f.label.empty() && running) {
      SkPoint mid;
      SkVector tan;
      if (cm->getPosTan(len * f.label_at, &mid, &tan)) {
        const SkFont font = fonts_->Bold(11.5f);
        const float w = TextWidth(font, f.label) + 14;
        SkRect pill = SkRect::MakeXYWH(mid.fX - w / 2, mid.fY - 9, w, 18);
        if (std::fabs(tan.fY) > std::fabs(tan.fX)) pill.offset(f.label_side * (w / 2 + 8), 0);
        // Between neighbouring blocks the gap is too narrow: hang the label
        // under the blocks instead of over their names.
        if (LaneOf(f.from) == LaneOf(f.to) && std::abs(SlotOf(f.from) - SlotOf(f.to)) == 1) {
          pill.offsetTo(pill.fLeft, BlockRect(f.from).fBottom + 1);
        }
        pill.offsetTo(std::clamp(pill.fLeft, kDiagram.fLeft + 6, kDiagram.fRight - 6 - w),
                      pill.fTop);
        labels.push_back({pill, f.label, f.color});
      }
    }
  }
  // Labels last, above every trace.
  for (const Label& l : labels) {
    c->drawRRect(SkRRect::MakeRectXY(l.pill, 9, 9), Fill(0xF20B1620));
    c->drawRRect(SkRRect::MakeRectXY(l.pill, 9, 9), Stroke(l.color, 1.2f));
    DrawText(c, l.text, l.pill.centerX(), l.pill.fBottom - 5, fonts_->Bold(11.5f), l.color,
             Align::kCenter);
  }
}

void Scene::DrawSnapshotBand(SkCanvas* c, const FrameModel& m) {
  auto visible = [&](int stage, float at) {
    return m.cursor.stage > stage || (m.cursor.stage == stage && m.cursor.progress >= at);
  };
  // Left: the snapshot file, to scale.
  const float top = 504;
  const float fw = DrawText(c, "tektronix/standardImage", 44, top + 14, fonts_->Bold(13.5f),
                            kText);
  DrawText(c, "755,354 bytes, as loaded by ObjectMemory", 44 + fw + 10, top + 14,
           fonts_->Regular(11.5f), kFaint);
  const SkRect bar = SkRect::MakeLTRB(44, top + 24, 560, top + 40);
  c->drawRRect(SkRRect::MakeRectXY(bar, 4, 4), Fill(0xFF12212C));
  float y = bar.fBottom + 20;
  bool any = false;
  for (const FileRegion& r : ImageFileMap()) {
    if (!visible(r.stage, r.at)) continue;
    any = true;
    const float x0 = bar.fLeft + bar.width() * static_cast<float>(r.base) / kImageFileSize;
    const float x1 = std::max(x0 + 3.f, bar.fLeft + bar.width() *
                                            static_cast<float>(r.base + r.size) / kImageFileSize);
    c->drawRect(SkRect::MakeLTRB(x0, bar.fTop, x1, bar.fBottom), Fill(WithAlpha(r.color, 0.9f)));
    const bool fresh = m.cursor.stage == r.stage;
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(44, y - 9, 10, 10), 2, 2), Fill(r.color));
    char range[48];
    std::snprintf(range, sizeof range, "%7llu - %7llu",
                  static_cast<unsigned long long>(r.base),
                  static_cast<unsigned long long>(r.base + r.size - 1));
    DrawText(c, range, 62, y, fonts_->Mono(11.5f), fresh ? kText : kMuted);
    DrawText(c, r.label, 212, y, fonts_->Regular(12), fresh ? kText : kMuted);
    y += 16;
  }
  if (!any) {
    DrawText(c, "(not opened yet)", 44, y, fonts_->Regular(12), kFaint);
  }

  // Right: objects the VM knows by oop.
  const float rx = 588;
  c->drawLine(rx - 14, top + 2, rx - 14, 632, Stroke(kPanelBorder, 1.f));
  const float kw = DrawText(c, "Known to the VM", rx, top + 14, fonts_->Bold(13.5f), kText);
  DrawText(c, "oops found by init() and later", rx + kw + 10, top + 14, fonts_->Regular(11.5f),
           kFaint);
  float cx = rx, cy = top + 26;
  const SkFont name_font = fonts_->Bold(11);
  const SkFont oop_font = fonts_->Mono(10.5f);
  bool shown = false;
  for (const KnownOop& k : KnownOops()) {
    if (!visible(k.stage, k.at)) continue;
    shown = true;
    const float w = TextWidth(name_font, k.name) + TextWidth(oop_font, k.oop) + 24;
    if (cx + w > kLaneRight) {
      cx = rx;
      cy += 26;
    }
    const bool fresh = m.cursor.stage == k.stage;
    const SkColor col = fresh ? BootStages()[k.stage].accent : 0xFF4F6B7D;
    const SkRect chip = SkRect::MakeXYWH(cx, cy, w, 20);
    c->drawRRect(SkRRect::MakeRectXY(chip, 10, 10), Fill(0xFF0F1D28));
    c->drawRRect(SkRRect::MakeRectXY(chip, 10, 10), Stroke(col, 1.1f));
    const float nw = DrawText(c, k.name, cx + 9, cy + 14, name_font, fresh ? kText : kMuted);
    DrawText(c, k.oop, cx + 15 + nw, cy + 14, oop_font, col);
    cx += w + 6;
  }
  if (!shown) {
    DrawText(c, "(nothing yet: the image is not loaded)", rx, top + 40, fonts_->Regular(12),
             kFaint);
  }
}

void Scene::DrawInfo(SkCanvas* c, const FrameModel& m) {
  const auto& stages = BootStages();
  const Stage& st = stages[m.cursor.stage];
  const SkRect r = SkRect::MakeLTRB(1020, 140, 1576, 520);
  Panel(c, r);
  c->drawRRect(
      SkRRect::MakeRectXY(SkRect::MakeLTRB(r.fLeft, r.fTop + 14, r.fLeft + 4, r.fTop + 70), 2, 2),
      Fill(st.accent));
  char tag[48];
  std::snprintf(tag, sizeof tag, "STAGE %d OF %d", m.cursor.stage + 1,
                static_cast<int>(stages.size()));
  DrawText(c, tag, r.fLeft + 22, r.fTop + 28, fonts_->Bold(12), st.accent);
  float y = r.fTop + 56;
  for (const std::string& line : Wrap(fonts_->Bold(21), st.title, r.width() - 44)) {
    DrawText(c, line, r.fLeft + 22, y, fonts_->Bold(21), kText);
    y += 26;
  }
  const float lw = DrawText(c, "Runs on:", r.fLeft + 22, y + 2, fonts_->Bold(12.5f), kMuted) + 6;
  for (const std::string& line : Wrap(fonts_->Regular(12.5f), st.actor, r.width() - 44 - lw)) {
    DrawText(c, line, r.fLeft + 22 + lw, y + 2, fonts_->Regular(12.5f), kMuted);
    y += 17;
  }
  y += 10;

  const int n = static_cast<int>(st.steps.size());
  const int current = std::min(n - 1, static_cast<int>(m.cursor.progress * n));
  // Largest body size whose wrapped steps fit above the source line.
  float size = 14.f, lh = 18.f;
  for (; size > 11.f; size -= 0.5f) {
    lh = size * 1.28f;
    float h = 0;
    for (const std::string& step : st.steps) {
      h += Wrap(fonts_->Regular(size), step, r.width() - 70).size() * lh + 6;
    }
    if (y + h <= r.fBottom - 34) break;
  }
  const SkFont body = fonts_->Regular(size);
  for (int i = 0; i < n; ++i) {
    const bool now = i == current;
    const SkColor col = i <= current ? kText : kMuted;
    const float bx = r.fLeft + 22;
    c->drawCircle(bx + 9, y - 5, 9, Fill(now ? st.accent : (i < current ? 0xFF2A4152 : 0xFF182A36)));
    char num[12];
    std::snprintf(num, sizeof num, "%d", i + 1);
    DrawText(c, num, bx + 9, y - 1, fonts_->Bold(11), now ? kInk : kMuted, Align::kCenter);
    for (const std::string& line : Wrap(body, st.steps[i], r.width() - 70)) {
      DrawText(c, line, bx + 26, y, body, col);
      y += lh;
    }
    y += 6;
  }
  const SkFont mono = fonts_->Mono(12);
  const std::string where = Ellipsize(mono, st.where, r.width() - 100);
  DrawText(c, "Source:", r.fLeft + 22, r.fBottom - 16, fonts_->Bold(12), kMuted);
  DrawText(c, where, r.fLeft + 78, r.fBottom - 16, mono, st.accent);
}

void Scene::DrawScreen(SkCanvas* c, const FrameModel& m) {
  const Stage& st = BootStages()[m.cursor.stage];
  const SkRect r = SkRect::MakeLTRB(1020, 532, 1576, 884);
  Panel(c, r);
  const float dw =
      DrawText(c, "The Smalltalk display", r.fLeft + 18, r.fTop + 24, fonts_->Bold(14.5f), kText);
  DrawText(c, "the bitmap as recorded from the VM", r.fLeft + 18 + dw + 10, r.fTop + 24,
           fonts_->Regular(11.5f), kFaint);

  const SkRect screen = SkRect::MakeXYWH(r.fLeft + 18, r.fTop + 38, 296, 296);
  const Screen* shot = ScreenAt(m.cursor);
  sk_sp<SkImage> image = shot ? ScreenImage(shot->cycle) : nullptr;
  c->drawRect(screen.makeOutset(4, 4), Fill(0xFF1A2833));
  if (image) {
    const SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    c->drawImageRect(image, SkRect::MakeWH(image->width(), image->height()), screen, sampling,
                     nullptr, SkCanvas::kFast_SrcRectConstraint);
    const float k = screen.width() / 1024.f;  // display pixels -> design units
    // Rectangles just reported to display_changed().
    for (const Damage& d : st.damage) {
      const float age = m.cursor.progress - d.at;
      if (age < 0 || age > 0.07f) continue;
      const SkRect dr = SkRect::MakeXYWH(screen.fLeft + d.x * k, screen.fTop + d.y * k,
                                         std::max(1.f, d.w * k), std::max(1.f, d.h * k));
      c->drawRect(dr.makeOutset(1.5f, 1.5f), Stroke(WithAlpha(st.accent, 1.f - age / 0.07f), 2));
    }
    if (st.pointer && m.cursor.progress >= 0.26f) {
      const float px = screen.fLeft + 800 * k, py = screen.fTop + 850 * k;
      SkPathBuilder arrow;
      arrow.moveTo(px, py);
      arrow.lineTo(px, py + 13);
      arrow.lineTo(px + 3.5f, py + 10);
      arrow.lineTo(px + 6.5f, py + 16);
      arrow.lineTo(px + 8.5f, py + 15);
      arrow.lineTo(px + 5.5f, py + 9);
      arrow.lineTo(px + 10, py + 9);
      arrow.close();
      const SkPath a = arrow.detach();
      c->drawPath(a, Fill(0xFF000000));
      c->drawPath(a, Stroke(0xFFFFFFFF, 1.2f));
    }
  } else {
    c->drawRect(screen, Fill(0xFF070D12));
    DrawText(c, "no window yet", screen.centerX(), screen.centerY() - 6, fonts_->Bold(14),
             kFaint, Align::kCenter);
    DrawText(c, "SDLHAL opens it at the first beDisplay", screen.centerX(), screen.centerY() + 14,
             fonts_->Regular(11.5f), kFaint, Align::kCenter);
  }

  // Facts beside the screen.
  const float x = screen.fRight + 20;
  float y = r.fTop + 58;
  auto fact = [&](const char* label, const std::string& value, SkColor col) {
    DrawText(c, label, x, y, fonts_->Regular(11.5f), kFaint);
    DrawText(c, value, x, y + 20, fonts_->Bold(17), col);
    y += 48;
  };
  const long long cycles = CyclesAt(m.cursor);
  fact("bytecodes executed", Grouped(cycles), kText);
  char ms[32];
  std::snprintf(ms, sizeof ms, "%.1f ms", static_cast<double>(cycles) / kCyclesPerMs);
  fact("virtual time (4,000 / ms)", ms, kMuted);
  fact("display bitmap", image ? "1024 × 1024 × 1 bit" : "none", image ? kText : kFaint);
  fact("shown", shot ? "after " + Grouped(shot->cycle) : std::string("-"), kMuted);
  if (image) {
    DrawText(c, "Grey is the desktop's", x, y - 4, fonts_->Regular(11), kFaint);
    DrawText(c, "halftone, scaled down.", x, y + 11, fonts_->Regular(11), kFaint);
  }
}

void Scene::DrawTrace(SkCanvas* c, const FrameModel& m) {
  const Stage& st = BootStages()[m.cursor.stage];
  const SkRect r = SkRect::MakeLTRB(24, 658, 1004, 884);
  c->drawRRect(SkRRect::MakeRectXY(r, 12, 12), Fill(0xF2050B10));
  c->drawRRect(SkRRect::MakeRectXY(r.makeInset(0.5f, 0.5f), 12, 12), Stroke(kPanelBorder, 1.f));
  c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(r.fLeft, r.fTop, r.fRight, r.fTop + 28), 12,
                                   12),
               Fill(0xFF0F1D28));
  c->drawRect(SkRect::MakeLTRB(r.fLeft, r.fTop + 16, r.fRight, r.fTop + 28), Fill(0xFF0F1D28));
  for (int i = 0; i < 3; ++i) {
    c->drawCircle(r.fLeft + 18 + i * 16, r.fTop + 14, 5,
                  Fill(i == 0 ? 0xFFFF6B6B : i == 1 ? 0xFFFFC75F : 0xFF6BE38A));
  }
  DrawText(c, "hal_trace · bytecodes · ms · kind · call", r.fLeft + 72, r.fTop + 19,
           fonts_->Bold(12.5f), kMuted);
  DrawText(c, "recorded from //:hal_trace booting tektronix/standardImage", r.fRight - 16,
           r.fTop + 19, fonts_->Regular(11.5f), kFaint, Align::kRight);

  const std::vector<std::string> lines = TraceUpTo(m.cursor);
  const SkFont mono = fonts_->Mono(12);
  const float line_h = 15.5f;
  const float top = r.fTop + 46;
  const int max_lines = static_cast<int>((r.fBottom - 8 - top) / line_h) + 1;
  const int first = std::max(0, static_cast<int>(lines.size()) - max_lines);
  float y = top;
  for (int i = first; i < static_cast<int>(lines.size()); ++i) {
    SkColor col = TraceColor(lines[i]);
    if (i == static_cast<int>(lines.size()) - 1) col = Mix(st.accent, col, 0.3f);
    DrawText(c, Ellipsize(mono, lines[i], r.width() - 32), r.fLeft + 16, y, mono, col);
    y += line_h;
  }
  if (!lines.empty() && std::fmod(m.time, 1.f) < 0.55f && y < r.fBottom - 4) {
    c->drawRect(SkRect::MakeXYWH(r.fLeft + 16, y - 11, 7, 13), Fill(0xFFE6EEF5));
  }
}

}  // namespace bootviz::scene
