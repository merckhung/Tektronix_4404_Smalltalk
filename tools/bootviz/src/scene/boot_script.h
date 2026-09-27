// How the Tektronix 4404 Smalltalk-80 image boots on this VM and how its
// calls reach the host, as a sequence of stages: who runs, what happens,
// which parts of the system are involved, what moves between them, what the
// display shows and what the recorded trace prints.
//
// The content follows the VM's sources (main_sdl.cpp, src/objmemory.cpp,
// src/interpreter.cpp, src/bitblt.cpp), the image's own Smalltalk sources
// (tektronix/system/standardSource: SystemDictionary>>snapshotAs:thenQuit:,
// SystemDictionary>>install, TekSystemCall, Subtask class>>install) and a
// recording made with //:hal_trace (tools/record_trace.sh ->
// data/tektronix_trace.txt, data/screens/). Trace lines keep the recorder's
// columns: bytecodes executed, virtual milliseconds, kind, call. Lines that
// start with '#' are the recorder's own summaries or annotations; '$' lines
// are shell commands.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bootviz::scene {

// Blocks of the architecture diagram, in three lanes.
enum class Part {
  // The Smalltalk-80 image: objects whose methods run as bytecodes.
  kProcessor,    // ProcessorScheduler, the active Process
  kControllers,  // ScheduledControllers: windows, menus
  kBitBltObj,    // BitBlt, DisplayScanner
  kDisplayObj,   // Display: the DisplayScreen form
  kSensor,       // Sensor: InputSensor + InputState process
  kTekCall,      // TekSystemCall: UniFLEX and display-driver calls
  // The C++ virtual machine (src/).
  kObjMem,     // ObjectMemory: object table + heap, GC
  kInterp,     // Interpreter::cycle(): fetch, decode, execute
  kSend,       // message send: method cache, lookup, contexts
  kPrims,      // dispatchPrimitives()
  kBitBltEng,  // the C++ BitBlt (src/bitblt.cpp)
  kSignals,    // asynchronousSignal(): semaphores from the host
  // The host (main_sdl.cpp, Linux, SDL2).
  kImageFile,  // tektronix/standardImage
  kFs,         // IFileSystem: PosixST80FileSystem
  kMain,       // main() and its event loop
  kHal,        // IHardwareAbstractionLayer: SDLHAL
  kSdl,        // SDL2 window, renderer, texture
  kInput,      // mouse and keyboard (SDL events)
  kCount
};

constexpr int kLaneCount = 3;
constexpr int kPartsPerLane = 6;
inline int LaneOf(Part p) { return static_cast<int>(p) / kPartsPerLane; }
inline int SlotOf(Part p) { return static_cast<int>(p) % kPartsPerLane; }

// Something moving between two parts during [start, end] of a stage
// (fractions of the stage).
struct Flow {
  Part from, to;
  std::string label;
  float start = 0.f, end = 1.f;
  uint32_t color = 0xFF7FD8FF;  // ARGB
  float label_at = 0.5f;        // where the label sits along the path
  int label_side = 1;           // on vertical runs: +1 right of the trace, -1 left
};

// A byte range of the snapshot file, shown from `stage` on.
struct FileRegion {
  uint64_t base, size;
  std::string label;
  uint32_t color;
  int stage;
  float at = 0.f;  // appears at this fraction of `stage`
};

// An object the VM must know by oop, found in `stage`.
struct KnownOop {
  std::string name;
  std::string oop;
  int stage;
  float at;
};

// The display bitmap as recorded after `cycle` bytecodes
// (data/screens/screen_<cycle>.png), shown from `at` in `stage`.
struct Screen {
  int stage;
  float at;
  long long cycle;
};

// A rectangle reported to hal->display_changed(), highlighted briefly.
struct Damage {
  float at;
  int x, y, w, h;
};

struct LogLine {
  float at;  // fraction of the stage
  std::string text;
};

struct Stage {
  std::string name;   // short, for the stage rail
  std::string title;  // headline
  std::string actor;  // who executes this stage
  std::string where;  // source reference
  std::vector<std::string> steps;
  float duration;     // seconds at 1x speed
  uint32_t accent;    // ARGB
  std::vector<Part> active;
  std::vector<Flow> flows;
  std::vector<LogLine> log;
  std::vector<Damage> damage;
  long long cycle_from = 0, cycle_to = 0;  // bytecodes executed, from the trace
  bool pointer = false;                    // show the mouse pointer on the screen
  float showcase = 0.75f;                  // a representative moment, for screenshots
};

const std::vector<Stage>& BootStages();
const std::vector<FileRegion>& ImageFileMap();
const std::vector<KnownOop>& KnownOops();
const std::vector<Screen>& Screens();

// Size of tektronix/standardImage in bytes.
constexpr uint64_t kImageFileSize = 755354;
// Bytecodes per virtual millisecond in the recording.
constexpr long long kCyclesPerMs = 4000;

// Total length of the sequence at 1x speed.
float TotalDuration();

// Position in the sequence: stage index and progress within it (0..1).
struct Cursor {
  int stage = 0;
  float progress = 0.f;
};
Cursor CursorAt(float seconds);
float SecondsAt(Cursor c);

// Trace lines printed up to `c`, oldest first.
std::vector<std::string> TraceUpTo(Cursor c);

// The recorded screen to show at `c`, or nullptr before the display exists.
const Screen* ScreenAt(Cursor c);

// Bytecodes executed at `c` (interpolated within the stage).
long long CyclesAt(Cursor c);

}  // namespace bootviz::scene
