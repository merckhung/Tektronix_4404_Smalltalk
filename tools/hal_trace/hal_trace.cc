// hal_trace: boots a Smalltalk-80 image headlessly and records every call the
// image makes to the host -- the primitives it runs and the hardware
// abstraction layer (HAL) and file-system calls those primitives make.
//
// The recording is what tools/bootviz replays: the boot phases, the first
// occurrence of each primitive, the HAL call log and the display contents at
// a few moments. Time is virtual (a fixed number of bytecodes per
// millisecond) so a run is repeatable.
//
//   bazel run //:hal_trace -- [-xerox] [-cycles N] [-click MS X Y]
//                             [-display-at CYCLE FILE.pbm]... > trace.txt
//
// Run it from the repository root, like the VM itself.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "interpreter.h"
#include "posixfilesystem.h"

namespace {

// Virtual clock: bytecodes per millisecond.
constexpr long long kCyclesPerMs = 4000;

// Mouse button / event encoding, as in main_sdl.cpp.
constexpr uint16_t kEventDeltaTime = 0, kEventMouseX = 1, kEventMouseY = 2, kEventKeyDown = 3,
                   kEventKeyUp = 4;
constexpr int kYellowButton = 129;

uint16_t EventWord(uint16_t type, int param) {
  return static_cast<uint16_t>((type << 12) | (param & 0x0FFF));
}

Interpreter* g_interp = nullptr;

long long Cycle() { return g_interp ? g_interp->cycles() : 0; }
double Ms() { return static_cast<double>(Cycle()) / kCyclesPerMs; }

// One trace line: "<cycle> <ms> <kind> <text>".
void Log(const char* kind, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Log(const char* kind, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::printf("%10lld %9.1f %-4s %s\n", Cycle(), Ms(), kind, buf);
}

// Calls that happen thousands of times are logged the first few times, then
// only counted.
struct Throttle {
  std::map<std::string, long long> counts;
  bool Allow(const std::string& key, long long first = 3) { return ++counts[key] <= first; }
};
Throttle g_throttle;

// After a mouse click, the next calls are logged even if their kind was
// already throttled, to show how the image reacts to the input.
int g_burst = 0;

class TraceHAL : public IHardwareAbstractionLayer {
 public:
  TraceHAL(ImageType type, std::string image) : type_(type), image_(std::move(image)) {}

  ImageType get_image_type() override { return type_; }

  void set_input_semaphore(int semaphore) override {
    input_semaphore_ = semaphore;
    Log("hal", "set_input_semaphore(%d)", semaphore);
  }
  uint32_t get_smalltalk_epoch_time() override {
    // 2026-01-01 00:00 UTC, in seconds since 1901-01-01.
    const uint32_t t = 1767225600u + 2208988800u;
    if (g_throttle.Allow("epoch")) Log("hal", "get_smalltalk_epoch_time() -> %u", t);
    return t;
  }
  uint32_t get_msclock() override {
    const uint32_t ms = static_cast<uint32_t>(Ms());
    if (g_throttle.Allow("msclock")) Log("hal", "get_msclock() -> %u", ms);
    return ms;
  }
  void signal_at(int semaphore, uint32_t ms) override {
    if (g_throttle.Allow("signal_at", 12)) Log("hal", "signal_at(sem=%d, at=%u ms)", semaphore, ms);
    timer_semaphore_ = semaphore;
    timer_ms_ = ms;
  }
  void set_cursor_image(uint16_t* image) override {
    if (g_throttle.Allow("cursor_image", 6)) {
      Log("hal", "set_cursor_image(16x16: %04x %04x %04x %04x ...)", image[0], image[1], image[2],
          image[3]);
    }
  }
  void set_cursor_location(int x, int y) override {
    mouse_x_ = x;
    mouse_y_ = y;
    if (g_throttle.Allow("cursor_loc")) Log("hal", "set_cursor_location(%d, %d)", x, y);
  }
  void get_cursor_location(int* x, int* y) override {
    *x = mouse_x_;
    *y = mouse_y_;
    if (g_throttle.Allow("get_cursor")) Log("hal", "get_cursor_location() -> (%d, %d)", *x, *y);
  }
  void set_link_cursor(bool link) override { Log("hal", "set_link_cursor(%s)", link ? "true" : "false"); }
  bool set_display_size(int w, int h) override {
    width_ = w;
    height_ = h;
    Log("hal", "set_display_size(%d, %d)", w, h);
    return true;
  }
  void display_changed(int x, int y, int w, int h) override {
    ++display_changes_;
    if (g_throttle.Allow("display_changed", 8) || g_burst > 0) {
      Log("hal", "display_changed(x=%d, y=%d, w=%d, h=%d)", x, y, w, h);
    }
  }
  bool next_input_word(uint16_t* word) override {
    if (input_.empty()) return false;
    *word = input_.front();
    input_.pop_front();
    Log("hal", "next_input_word() -> 0x%04x (type %d, param %d)", *word, *word >> 12, *word & 4095);
    return true;
  }
  void error(const char* message) override {
    Log("err", "%s", message);
    std::fflush(stdout);
    std::exit(1);
  }
  void signal_quit() override { Log("hal", "signal_quit()"); }
  void exit_to_debugger() override { Log("hal", "exit_to_debugger()"); }
  const char* get_image_name() override { return image_.c_str(); }
  void set_image_name(const char* name) override {
    image_ = name;
    Log("hal", "set_image_name(\"%s\")", name);
  }

  // Host side of the main loop.
  void CheckTimer() {
    if (timer_semaphore_ && static_cast<int32_t>(static_cast<uint32_t>(Ms()) - timer_ms_) >= 0) {
      const int sem = timer_semaphore_;
      timer_semaphore_ = 0;
      if (g_throttle.Allow("timer_fire", 12)) Log("host", "timer due: asynchronousSignal(%d)", sem);
      g_interp->asynchronousSignal(sem);
    }
  }
  void Push(uint16_t word) {
    if (!input_semaphore_) return;
    input_.push_back(word);
    Log("host", "input word 0x%04x queued, asynchronousSignal(%d)", word, input_semaphore_);
    g_interp->asynchronousSignal(input_semaphore_);
  }
  void Click(int x, int y, int button) {
    Log("host", "mouse: button %d down at (%d, %d)", button, x, y);
    g_burst = 40;
    mouse_x_ = x;
    mouse_y_ = y;
    Push(EventWord(kEventDeltaTime, 0));
    Push(EventWord(kEventMouseX, x));
    Push(EventWord(kEventMouseY, y));
    Push(EventWord(kEventDeltaTime, 0));
    Push(EventWord(kEventKeyDown, button));
  }
  void Release(int button) {
    Log("host", "mouse: button %d up", button);
    Push(EventWord(kEventDeltaTime, 120));
    Push(EventWord(kEventKeyUp, button));
  }

  bool SaveDisplay(const std::string& path) {
    const int bits = g_interp->getDisplayBits(width_, height_);
    if (!bits) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P4\n%d %d\n", width_, height_);
    const int words = (width_ + 15) / 16;
    const int bytes = (width_ + 7) / 8;
    for (int row = 0; row < height_; ++row) {
      for (int b = 0; b < bytes; ++b) {
        const uint16_t w = g_interp->fetchWord_ofDislayBits(row * words + b / 2, bits);
        std::fputc((b & 1) ? (w & 0xFF) : (w >> 8), f);
      }
    }
    std::fclose(f);
    const size_t slash = path.rfind('/');
    Log("host", "display %dx%d saved (%s)", width_, height_,
        path.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
    return true;
  }

  long long display_changes_ = 0;

 private:
  ImageType type_;
  std::string image_;
  int input_semaphore_ = 0;
  int timer_semaphore_ = 0;
  uint32_t timer_ms_ = 0;
  int mouse_x_ = 0, mouse_y_ = 0;
  int width_ = 0, height_ = 0;
  std::deque<uint16_t> input_;
};

// Logs the file-system calls the image makes (sources, changes, snapshot).
class TraceFS : public IFileSystem {
 public:
  explicit TraceFS(IFileSystem* fs) : fs_(fs) {}
  int create_file(const char* name) override {
    const int r = fs_->create_file(name);
    Log("fs", "create_file(\"%s\") -> %d", name, r);
    return r;
  }
  int open_file(const char* name) override {
    const int r = fs_->open_file(name);
    Log("fs", "open_file(\"%s\") -> %d", name, r);
    return r;
  }
  int close_file(int fd) override {
    Log("fs", "close_file(%d)", fd);
    return fs_->close_file(fd);
  }
  int seek_to(int fd, int pos) override {
    if (g_throttle.Allow("seek", 6)) Log("fs", "seek_to(%d, %d)", fd, pos);
    return fs_->seek_to(fd, pos);
  }
  int tell(int fd) override { return fs_->tell(fd); }
  int read(int fd, char* buf, int n) override {
    const int r = fs_->read(fd, buf, n);
    if (g_throttle.Allow("read", 6)) Log("fs", "read(%d, %d bytes) -> %d", fd, n, r);
    return r;
  }
  int write(int fd, const char* buf, int n) override {
    const int r = fs_->write(fd, buf, n);
    if (g_throttle.Allow("write", 6)) Log("fs", "write(%d, %d bytes) -> %d", fd, n, r);
    return r;
  }
  bool truncate_to(int fd, int len) override { return fs_->truncate_to(fd, len); }
  int file_size(int fd) override {
    const int r = fs_->file_size(fd);
    if (g_throttle.Allow("file_size")) Log("fs", "file_size(%d) -> %d", fd, r);
    return r;
  }
  bool file_flush(int fd) override { return fs_->file_flush(fd); }
  void enumerate_files(const std::function<void(const char*)>& each) override {
    Log("fs", "enumerate_files()");
    fs_->enumerate_files(each);
  }
  bool rename_file(const char* a, const char* b) override { return fs_->rename_file(a, b); }
  bool delete_file(const char* name) override { return fs_->delete_file(name); }
  const int last_error() override { return fs_->last_error(); }
  const char* error_text(int code) override { return fs_->error_text(code); }

 private:
  IFileSystem* fs_;
};

// TekSystemCall operation codes, from the class side of TekSystemCall in
// tektronix/system/standardSource ('display operations' go through
// primitive 135, 'system dependent operations' through primitive 134).
const char* TekOperationName(bool display, int op) {
  static const std::map<int, const char*> kDisplay = {
      {0, "cursorOn"}, {1, "cursorOff"}, {4, "enableCursorPanning"},
      {5, "disableCursorPanning"}, {6, "turnDisplayOn"}, {7, "turnDisplayOff"},
      {8, "enableJoydiskPanning"}, {9, "disableJoydiskPanning"}, {10, "timeOutOn"},
      {11, "timeOutOff"}, {12, "blackOnWhite"}, {13, "whiteOnBlack"}, {14, "terminalOn"},
      {15, "terminalOff"}, {26, "getViewPort"}, {27, "setViewPort:"}, {28, "getDisplayState:"},
      {29, "setKeyboardCode:"}, {30, "getMouseBounds"}, {31, "setMouseBounds:lowerRight:"},
      {40, "eventsEnable"}, {41, "eventsDisable"}, {42, "eventSignalOn"}, {47, "getAlarmTime"},
      {48, "setAlarmTime:"}, {49, "clearAlarm"}};
  static const std::map<int, const char*> kSystem = {
      {2, "exec:with:"}, {3, "fork"}, {4, "wait"}, {5, "term:"}, {8, "cpint:to:"},
      {9, "spint:an:"}, {10, "open:mode:"}, {11, "create:mode:"}, {12, "read:buffer:nbytes:"},
      {13, "write:buffer:nbytes:"}, {14, "seek:offset:whence:"}, {15, "close:"}, {16, "dup:"},
      {17, "dups:with:"}, {18, "link:to:"}, {19, "unlink:"}, {20, "crtsd:mode:addr:"},
      {21, "chdir:"}, {23, "chown:to:"}, {24, "chprm:to:"}, {25, "chacc:mode:"},
      {26, "defacc:"}, {27, "ofstat:buffer:"}, {28, "status:buffer:"}, {31, "crPipe"},
      {32, "getId"}, {33, "getuId"}, {34, "setuId:"}, {35, "setpr:"}, {39, "time:"},
      {41, "ttime:"}, {42, "update"}, {43, "alarm:"}, {45, "ttyget:buffer:"},
      {46, "ttyset:buffer:"}, {47, "lrec:howmany:"}, {48, "urec:"}, {51, "ttynumber"},
      {52, "filtime:to:"}, {55, "truncate:"}, {56, "vfork"}, {59, "execve:withArgs:withEnv:"},
      {62, "createPty"}, {65, "controlPty:command:mode:"}, {66, "rump:operation:"},
      {69, "fcntl:function:"}, {16383, "invocationArgCountAddress"}};
  const auto& table = display ? kDisplay : kSystem;
  const auto it = table.find(op);
  return it == table.end() ? "?" : it->second;
}

// Per-primitive statistics, printed at the end.
struct PrimStats {
  long long calls = 0, failures = 0;
  long long first_cycle = -1;
  std::string selector, receiver;
};
std::map<int, PrimStats> g_prims;

// Primitives that talk to the host (I/O, system, Tektronix/private) are
// logged the first few times; the rest (arithmetic, storage, control) only
// on their first use.
void OnPrimitive(void*, const Interpreter::PrimitiveCall& c) {
  PrimStats& s = g_prims[c.index];
  if (s.calls == 0) {
    s.first_cycle = c.cycle;
    s.selector = c.selector;
    s.receiver = c.receiverClass;
  }
  ++s.calls;
  if (!c.succeeded) ++s.failures;
  const bool host = c.index >= 90;
  const long long limit = host ? (c.index == 90 ? 3 : c.index == 96 ? 6 : 12) : 1;
  const bool burst = host && g_burst > 0 && c.index != 90;
  if (burst) --g_burst;
  if (c.index == 134 || c.index == 135) {
    // TekSystemCall: operationType operation D0In D0Out D1In D1Out D2In A0In A0Out errno.
    // The VM does not run UniFLEX: it answers true and zeroes the Outs.
    const int op = g_interp->fieldOf(c.receiver, 1);
    const int opv = std::atoi(g_interp->printOop(op).c_str());
    std::string args;
    static const char* kIns[] = {"D0In", "D1In", "D2In", "A0In"};
    static const int kInFields[] = {2, 4, 6, 7};
    for (int i = 0; i < 4; ++i) {
      const std::string v = g_interp->printOop(g_interp->fieldOf(c.receiver, kInFields[i]));
      if (v != "nil") args += std::string(" ") + kIns[i] + "=" + v;
    }
    // Indexed arguments (operation:with:with:), after the 10 named fields.
    for (int i = 10; i < g_interp->fieldCountOf(c.receiver); ++i) {
      args += " arg" + std::to_string(i - 9) + "=" +
              g_interp->printOop(g_interp->fieldOf(c.receiver, i));
    }
    Log("tek", "#%d %s op %d = TekSystemCall %s%s -> stubbed: true, Outs = 0", c.index,
        c.selector, opv, TekOperationName(c.index == 135, opv), args.c_str());
    return;
  }
  if (s.calls <= limit || burst) {
    Log("prim", "#%d %s>>%s%s", c.index, c.receiverClass, c.selector,
        c.succeeded ? "" : "  [failed: falls back to Smalltalk code]");
  }
}

uint32_t Be32(const unsigned char* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

void DescribeImage(const std::string& path, bool tek) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;
  unsigned char h[8];
  const size_t n = std::fread(h, 1, 8, f);
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fclose(f);
  if (n != 8) return;
  std::printf("# image %s: %ld bytes\n", path.c_str(), size);
  if (tek) {
    const uint32_t data_words = Be32(h);
    const long ot = 512 + static_cast<long>(data_words) * 2;
    std::printf("# header: object space %u words (%u bytes) at offset 512\n", data_words,
                data_words * 2);
    std::printf("# object table at offset %ld: %ld entries of 4 bytes\n", ot, (size - ot) / 4);
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool tek = true;
  std::string image = "tektronix/standardImage";
  long long max_cycles = 30'000'000;
  long long click_ms = -1;
  int click_x = 0, click_y = 0;
  std::vector<std::pair<long long, std::string>> dumps;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-xerox") {
      tek = false;
      image = "files/snapshot.im";
    } else if (a == "-image" && i + 1 < argc) {
      image = argv[++i];
    } else if (a == "-cycles" && i + 1 < argc) {
      max_cycles = std::atoll(argv[++i]);
    } else if (a == "-click" && i + 3 < argc) {
      click_ms = std::atoll(argv[++i]);
      click_x = std::atoi(argv[++i]);
      click_y = std::atoi(argv[++i]);
    } else if (a == "-display-at" && i + 2 < argc) {
      const long long at = std::atoll(argv[++i]);
      dumps.emplace_back(at, argv[++i]);
      std::sort(dumps.begin(), dumps.end());
    } else {
      std::fprintf(stderr,
                   "usage: hal_trace [-xerox] [-image PATH] [-cycles N] [-click MS X Y]\n"
                   "                 [-display-at CYCLE FILE.pbm]...\n");
      return 2;
    }
  }

  TraceHAL hal(tek ? ImageType::Tektronix : ImageType::Xerox, image);
  PosixST80FileSystem posix(".");
  TraceFS fs(&posix);
  auto interp = std::make_unique<Interpreter>(&hal, &fs);
  g_interp = interp.get();

  DescribeImage(image, tek);
  Log("vm", "Interpreter::init(): load snapshot, calibrate known oops");
  if (!interp->init()) {
    Log("err", "init failed");
    return 1;
  }
  Log("vm", "resume the active process: first bytecode");
  interp->setPrimitiveObserver(OnPrimitive, nullptr);

  // Main loop, like main_sdl.cpp: timers, then a batch of bytecodes.
  bool clicked = false, released = false;
  size_t next_dump = 0;
  while (Cycle() < max_cycles) {
    hal.CheckTimer();
    if (click_ms >= 0 && !clicked && Ms() >= click_ms) {
      hal.Click(click_x, click_y, kYellowButton);
      clicked = true;
    }
    if (clicked && !released && Ms() >= click_ms + 400) {
      hal.Release(kYellowButton);
      released = true;
    }
    // Batches of 5000 bytecodes, cut short to land exactly on a -display-at.
    long long batch = 5000;
    if (next_dump < dumps.size()) batch = std::max(1LL, std::min(batch, dumps[next_dump].first - Cycle()));
    for (long long i = 0; i < batch; ++i) interp->cycle();
    while (next_dump < dumps.size() && Cycle() >= dumps[next_dump].first) {
      hal.SaveDisplay(dumps[next_dump].second);
      ++next_dump;
    }
  }

  std::printf("# summary: %lld bytecodes, %.1f virtual ms, %lld display updates\n", Cycle(), Ms(),
              hal.display_changes_);
  std::printf("# primitives used: index calls failures first-cycle receiver>>selector\n");
  for (const auto& [index, s] : g_prims) {
    std::printf("#   %3d %9lld %7lld %10lld %s>>%s\n", index, s.calls, s.failures, s.first_cycle,
                s.receiver.c_str(), s.selector.c_str());
  }
  std::printf("# host call counts\n");
  for (const auto& [key, n] : g_throttle.counts) std::printf("#   %-16s %lld\n", key.c_str(), n);
  return 0;
}
