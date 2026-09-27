#include "src/scene/boot_script.h"

#include <algorithm>
#include <cstdio>

namespace bootviz::scene {
namespace {

constexpr uint32_t kCyan = 0xFF5CD6FF;
constexpr uint32_t kAmber = 0xFFFFB547;
constexpr uint32_t kViolet = 0xFFB18CFF;
constexpr uint32_t kGreen = 0xFF6BE38A;
constexpr uint32_t kRose = 0xFFFF7A9A;
constexpr uint32_t kSky = 0xFF7FB2FF;
constexpr uint32_t kLime = 0xFFC6E86B;

using P = Part;

// A trace line in the recorder's columns: bytecodes, virtual ms, kind, text.
LogLine T(float at, long long cycle, const char* kind, const std::string& text) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%8lld %7.1f %-4s ", cycle,
                static_cast<double>(cycle) / kCyclesPerMs, kind);
  return {at, buf + text};
}

// An annotation or recorder summary.
LogLine Note(float at, const std::string& text) { return {at, "# " + text}; }

std::vector<Stage> MakeStages() {
  std::vector<Stage> s;

  s.push_back(Stage{
      .name = "Launch",
      .title = "main() wires the host to the virtual machine",
      .actor = "Host CPU: native C++ (main_sdl.cpp)",
      .where = "main_sdl.cpp: main()",
      .steps = {"Parses the flags: -tek (the default) picks tektronix/standardImage, "
                "-xerox picks files/snapshot.im.",
                "Creates SDLHAL, the host side of IHardwareAbstractionLayer: clock, timer, "
                "cursor, display and the input-word queue.",
                "Creates PosixST80FileSystem(\".\"), the IFileSystem every file the image "
                "opens goes through, the snapshot included.",
                "Creates the Interpreter with both interfaces. The VM core never calls SDL or "
                "POSIX itself: every host service is one of these calls."},
      .duration = 6.f,
      .accent = kSky,
      .active = {P::kMain, P::kHal, P::kFs, P::kInterp},
      .flows = {{P::kMain, P::kHal, "new SDLHAL", 0.20f, 0.45f, kSky},
                {P::kMain, P::kFs, "new PosixST80FileSystem(\".\")", 0.42f, 0.66f, kSky, 0.5f},
                {P::kMain, P::kInterp, "new Interpreter(&hal, &fs)", 0.66f, 0.96f, kSky, 0.5f,
                 -1}},
      .log = {{0.05f, "$ ./bazel-bin/smalltalk_tek        # -tek is the default"},
              Note(0.30f, "SDLHAL hal(ImageType::Tektronix, \"tektronix/standardImage\", false)"),
              Note(0.52f, "PosixST80FileSystem fs(\".\")"),
              Note(0.75f, "Interpreter interpreter(&hal, &fs); interpreter.init()")},
      .showcase = 0.85f,
  });

  s.push_back(Stage{
      .name = "Load image",
      .title = "ObjectMemory::loadSnapshot() reads the 1986 image",
      .actor = "Interpreter::init() → ObjectMemory (C++)",
      .where = "src/objmemory.cpp: loadSnapshot, loadObjectTable, loadObjects",
      .steps = {"Opens tektronix/standardImage (755,354 bytes) through IFileSystem.",
                "The 512-byte header holds 68000 big-endian words: the object space is "
                "295,501 words (591,002 bytes) starting at offset 512.",
                "The object table follows at offset 591,514: 40,960 four-byte entries, each "
                "with the pointer / odd-length / free flags and a 24-bit address.",
                "Every live object is copied into the VM's segmented heap, its words "
                "byte-swapped from the 68000's big-endian order."},
      .duration = 8.f,
      .accent = kViolet,
      .active = {P::kInterp, P::kObjMem, P::kFs, P::kImageFile},
      .flows = {{P::kInterp, P::kObjMem, "loadSnapshot()", 0.03f, 0.25f, kViolet},
                {P::kImageFile, P::kFs, "755,354 bytes", 0.18f, 0.92f, kViolet},
                {P::kFs, P::kObjMem, "header · object table · objects", 0.28f, 0.95f, kViolet,
                 0.5f, -1}},
      .log = {Note(0.02f, "image tektronix/standardImage: 755354 bytes"),
              T(0.08f, 0, "vm", "Interpreter::init(): load snapshot, calibrate known oops"),
              T(0.14f, 0, "fs", "open_file(\"tektronix/standardImage\") -> 3"),
              T(0.18f, 0, "fs", "file_size(3) -> 755354"),
              T(0.22f, 0, "fs", "seek_to(3, 0)"),
              T(0.26f, 0, "fs", "read(3, 8 bytes) -> 8"),
              Note(0.30f, "header: object space 295501 words (591002 bytes) at offset 512"),
              T(0.36f, 0, "fs", "seek_to(3, 591514)"),
              T(0.40f, 0, "fs", "read(3, 4 bytes) -> 4"),
              Note(0.44f, "object table at offset 591514: 40960 entries of 4 bytes"),
              T(0.56f, 0, "fs", "seek_to(3, 512)"),
              T(0.62f, 0, "fs", "seek_to(3, 516)"),
              Note(0.80f, "load totals: read 97776, seek 56972, file_size 18992 calls"),
              T(0.92f, 0, "fs", "close_file(3)")},
      .showcase = 0.7f,
  });

  s.push_back(Stage{
      .name = "Calibrate",
      .title = "Calibrate: find the objects the VM must know",
      .actor = "Interpreter::init() (C++), before any bytecode runs",
      .where = "src/interpreter.cpp: Interpreter::init",
      .steps = {"The Blue Book puts well-known objects at fixed oops. The Tektronix image "
                "moves many of them, so init() looks them up by name.",
                "Pass 1 finds the class named Metaclass. Pass 2 visits every object whose "
                "class is a Metaclass instance and reads its name (field 6): SmallInteger, "
                "CompiledMethod, String, Symbol, Semaphore, DisplayScreen, MethodContext...",
                "Fixed oops: nil 2, false 4, true 6, the Processor association 8, and the "
                "Smalltalk dictionary 32288, the garbage collector's root.",
                "ObjectMemory gets the calibrated SmallInteger and CompiledMethod classes, "
                "which its mark-sweep collector needs to trace method literals."},
      .duration = 7.f,
      .accent = kLime,
      .active = {P::kInterp, P::kObjMem},
      .flows = {{P::kInterp, P::kObjMem, "scan: class names", 0.08f, 0.55f, kLime},
                {P::kObjMem, P::kInterp, "known oops", 0.55f, 0.95f, kLime, 0.5f}},
      .log = {Note(0.05f, "init() prints nothing: these are the values it settles on"),
              Note(0.25f, "Metaclass found by name; classes = instances of Metaclass instances"),
              Note(0.50f, "NilPointer 2  FalsePointer 4  TruePointer 6"),
              Note(0.65f, "SchedulerAssociationPointer 8  SmalltalkPointer 32288"),
              Note(0.82f, "memory.ClassSmallInteger / ClassCompiledMethod <- calibrated oops")},
      .showcase = 0.9f,
  });

  s.push_back(Stage{
      .name = "Resume",
      .title = "Resume the process that saved the snapshot",
      .actor = "Interpreter::cycle(): the image's bytecodes",
      .where = "SystemDictionary>>snapshotAs:thenQuit: (standardSource)",
      .steps = {"Processor (the value of oop 8) → activeProcess → suspendedContext gives the "
                "active context; fetchContextRegisters loads its method, instruction and stack "
                "pointers.",
                "That context is SystemDictionary>>snapshotAs:thenQuit:, frozen on 9 October "
                "1986 when a user chose quit → save from the screen menu.",
                "The snapshot primitive now answers self, so the method takes its \"continue "
                "from a snapshot image\" branch: postSnapshot, Subtask install, restore.",
                "postSnapshot → SystemDictionary>>install: CompiledMethod, SmallInteger and "
                "LargeInteger initialize, then the display and the input sensor."},
      .duration = 7.f,
      .accent = kGreen,
      .active = {P::kObjMem, P::kInterp, P::kProcessor, P::kSend, P::kPrims},
      .flows = {{P::kObjMem, P::kInterp, "activeContext", 0.04f, 0.28f, kGreen},
                {P::kInterp, P::kProcessor, "resume activeProcess", 0.26f, 0.55f, kGreen, 0.5f,
                 -1},
                {P::kProcessor, P::kSend, "snapshotAs:thenQuit:", 0.52f, 0.80f, kGreen},
                {P::kSend, P::kPrims, "#79 newMethod:header:", 0.78f, 0.98f, kGreen}},
      .log = {Note(0.02f, "stack: SystemDictionary>>snapshotAs:thenQuit:"),
              Note(0.06f, "         <- ScreenController>>quit <- yellowButtonActivity"),
              Note(0.10f, "         <- controlActivity <- controlLoop <- startUp"),
              T(0.30f, 0, "vm", "resume the active process: first bytecode"),
              T(0.55f, 25, "prim", "#85 Semaphore>>signal"),
              T(0.78f, 40, "prim", "#79 CompiledMethod class>>newMethod:header:"),
              T(0.82f, 41, "prim", "#62 CompiledMethod>>size"),
              T(0.88f, 57, "prim", "#71 Array class>>new:"),
              T(0.94f, 65, "prim", "#2 SmallInteger>>subtractOrFail:")},
      .cycle_from = 0,
      .cycle_to = 830,
      .showcase = 0.62f,
  });

  s.push_back(Stage{
      .name = "Display on",
      .title = "Display beDisplay and InputSensor install",
      .actor = "Image → primitives → SDLHAL → SDL",
      .where = "SystemDictionary>>install; interpreter.cpp: primitiveBeDisplay",
      .steps = {"Display beDisplay is primitive 102. primitiveBeDisplay reads the form's "
                "width and height and calls hal->set_display_size(1024, 1024).",
                "SDLHAL creates the SDL window, a renderer and an RGB565 streaming texture "
                "the size of the Tektronix bitmap.",
                "InputSensor install makes an InputState, asks the display driver for its "
                "state (the first TekSystemCall), forks the InputState process and registers "
                "its Semaphore with primitive 93: hal->set_input_semaphore(19264).",
                "resetSpaceLimits asks oopsLeft (#115) and coreLeft (#112) and arms the "
                "low-space Semaphore (#116)."},
      .duration = 8.f,
      .accent = kCyan,
      .active = {P::kDisplayObj, P::kSensor, P::kPrims, P::kHal, P::kSdl, P::kSignals},
      .flows = {{P::kDisplayObj, P::kPrims, "#102 beDisplay", 0.04f, 0.26f, kCyan, 0.5f, -1},
                {P::kPrims, P::kHal, "set_display_size(1024, 1024)", 0.22f, 0.44f, kCyan},
                {P::kHal, P::kSdl, "SDL_CreateWindow", 0.40f, 0.56f, kCyan},
                {P::kSensor, P::kPrims, "#93 primInputSemaphore:", 0.54f, 0.74f, kCyan},
                {P::kPrims, P::kHal, "set_input_semaphore(19264)", 0.70f, 0.90f, kCyan}},
      .log = {T(0.20f, 831, "prim", "#102 DisplayScreen>>beDisplay"),
              T(0.24f, 831, "hal", "set_display_size(1024, 1024)"),
              T(0.50f, 848, "prim", "#70 InputState class>>new"),
              T(0.54f, 1051, "prim", "#86 Semaphore>>wait"),
              T(0.58f, 1193, "tek", "#135 displayInvoke op 28 = getDisplayState: A0In=a WordArray"),
              T(0.62f, 1310, "prim", "#87 Process>>resume"),
              T(0.68f, 1322, "prim", "#93 InputState>>primInputSemaphore:"),
              T(0.72f, 1322, "hal", "set_input_semaphore(19264)"),
              T(0.82f, 1595, "prim", "#115 SystemDictionary>>oopsLeft"),
              T(0.86f, 1600, "prim", "#112 SystemDictionary>>coreLeft"),
              T(0.94f, 3699, "prim", "#116 SystemDictionary>>signal:atOopsLeft:wordsLeft:")},
      .cycle_from = 831,
      .cycle_to = 3699,
      .showcase = 0.74f,
  });

  s.push_back(Stage{
      .name = "TekSystemCall",
      .title = "TekSystemCall: the image calls UniFLEX",
      .actor = "TekSystemCall → primitives 134 / 135 → the VM's stub",
      .where = "TekSystemCall (OS-Interface); interpreter.cpp: primitiveTekSystemCall",
      .steps = {"On a 4404 the image talks to the UniFLEX kernel through TekSystemCall "
                "objects: an operation number plus the 68000 registers D0, D1, D2 and A0.",
                "systemInvoke (primitive 134) is an operating-system call; displayInvoke "
                "(primitive 135) a display-driver call.",
                "Restoring makes seven: getDisplayState: twice, cpint:to: for interrupts 26 (child "
                "died) and 6 (broken pipe), invocationArgCountAddress, setpr: 10, getViewPort.",
                "No UniFLEX here: primitiveTekSystemCall answers true with D0Out, D1Out, A0Out "
                "and errno zeroed, and memoryAt: (#144) answers 0. The image carries on."},
      .duration = 9.f,
      .accent = kAmber,
      .active = {P::kTekCall, P::kPrims, P::kDisplayObj, P::kSensor},
      .flows = {{P::kTekCall, P::kPrims, "#134 cpint: 26 to: aSemaphore → true", 0.02f, 0.20f,
                 kAmber, 0.3f},
                {P::kTekCall, P::kPrims, "#134 invocationArgCountAddress → true", 0.20f, 0.34f,
                 kAmber, 0.3f},
                {P::kDisplayObj, P::kPrims, "#144 memoryAt: → 0", 0.34f, 0.46f, kAmber, 0.5f,
                 -1},
                {P::kTekCall, P::kPrims, "#134 cpint: 6 · setpr: 10 → true", 0.46f, 0.62f, kAmber,
                 0.3f},
                {P::kTekCall, P::kPrims, "#135 getDisplayState: · getViewPort → true", 0.62f,
                 0.80f, kAmber, 0.3f},
                {P::kPrims, P::kTekCall, "", 0.04f, 0.82f, kGreen},
                {P::kSensor, P::kPrims, "#91 cursorPoint: 0@0", 0.84f, 0.98f, kCyan, 0.5f}},
      .log = {T(0.04f, 3909, "tek", "#134 systemInvoke op 8 = cpint:to: arg1=26 arg2=a Semaphore"),
              T(0.22f, 4359, "tek", "#134 systemInvoke op 16383 = invocationArgCountAddress"),
              T(0.36f, 4370, "prim", "#144 DisplayBitmap>>memoryAt:   (x5: reads argc/env)"),
              T(0.48f, 4756, "tek", "#134 systemInvoke op 8 = cpint:to: arg1=6 arg2=a Semaphore"),
              T(0.56f, 5044, "tek", "#134 systemInvoke op 35 = setpr: D0In=10"),
              T(0.64f, 5130, "tek", "#135 displayInvoke op 28 = getDisplayState: A0In=a WordArray"),
              T(0.72f, 5209, "tek", "#135 displayInvoke op 26 = getViewPort"),
              Note(0.84f, "every TekSystemCall -> stubbed: true, D0Out = D1Out = A0Out = errno = 0"),
              T(0.92f, 5530, "prim", "#91 InputState>>primCursorLocPut:"),
              T(0.95f, 5530, "hal", "set_cursor_location(0, 0)")},
      .cycle_from = 3700,
      .cycle_to = 5800,
      .showcase = 0.28f,
  });

  s.push_back(Stage{
      .name = "Redraw",
      .title = "ScheduledControllers restore paints the desktop",
      .actor = "Image views → BitBlt primitives → SDLHAL → SDL",
      .where = "BitBlt>>copyBits (#96); src/bitblt.cpp; SDLHAL::display_changed",
      .steps = {"ScheduledControllers restore redraws every window, back to front: File List, "
                "System Transcript, System Workspace, System Browser.",
                "Each fill, border and label is BitBlt copyBits (primitive 96): the C++ BitBlt "
                "combines source, halftone and destination words in the 1024×1024 bitmap.",
                "Text goes through DisplayScanner scanCharacters (primitive 103), which blits "
                "glyphs from the font's strike form.",
                "After a copy into the display form the VM calls hal->display_changed(x, y, "
                "w, h); SDLHAL expands the 1-bit rows into the texture and presents it.",
                "Cursor normal show: beCursor (#101) → set_cursor_image. The desktop is done "
                "after about 89,000 bytecodes."},
      .duration = 12.f,
      .accent = kRose,
      .active = {P::kControllers, P::kBitBltObj, P::kPrims, P::kBitBltEng, P::kObjMem, P::kHal,
                 P::kSdl},
      .flows = {{P::kControllers, P::kBitBltObj, "restore", 0.02f, 0.20f, kRose},
                {P::kBitBltObj, P::kPrims, "#96 copyBits", 0.08f, 0.90f, kRose, 0.55f},
                {P::kPrims, P::kBitBltEng, "copyBits()", 0.12f, 0.90f, kRose},
                {P::kBitBltEng, P::kObjMem, "display bits", 0.16f, 0.90f, kRose, 0.5f},
                {P::kPrims, P::kHal, "display_changed(x, y, w, h)", 0.20f, 0.92f, kRose},
                {P::kHal, P::kSdl, "texture + present", 0.24f, 0.95f, kRose}},
      .log = {T(0.05f, 8063, "prim", "#96 BitBlt>>copyBits"),
              T(0.07f, 8063, "hal", "display_changed(x=0, y=0, w=1024, h=1024)"),
              T(0.15f, 8817, "hal", "display_changed(x=108, y=235, w=392, h=1)"),
              T(0.17f, 9028, "hal", "display_changed(x=108, y=788, w=392, h=1)"),
              T(0.19f, 9239, "hal", "display_changed(x=108, y=236, w=1, h=552)"),
              T(0.21f, 9450, "hal", "display_changed(x=499, y=236, w=1, h=552)"),
              T(0.23f, 9666, "hal", "display_changed(x=109, y=236, w=390, h=552)"),
              T(0.29f, 10454, "hal", "display_changed(x=110, y=215, w=57, h=20)"),
              T(0.37f, 12847, "prim", "#103 DisplayScanner>>scanCharactersFrom:to:in:rightX:"
                                      "stopConditions:displaying:"),
              T(0.52f, 21395, "prim", "#103 DisplayScanner>>scanCharactersFrom:to:in:..."),
              T(0.60f, 30441, "prim", "#103 DisplayScanner>>scanCharactersFrom:to:in:..."),
              T(0.74f, 40339, "prim", "#103 DisplayScanner>>scanCharactersFrom:to:in:..."),
              T(0.80f, 44804, "prim", "#103 DisplayScanner>>scanCharactersFrom:to:in:..."),
              T(0.93f, 89283, "prim", "#101 Cursor>>beCursor"),
              T(0.95f, 89283, "hal", "set_cursor_image(16x16: 8000 c000 e000 f000 ...)"),
              Note(0.97f, "desktop drawn: 137 copyBits, 44 scanCharacters, 168 display_changed")},
      .damage = {{0.05f, 0, 0, 1024, 1024},  {0.15f, 108, 235, 392, 1},
                 {0.17f, 108, 788, 392, 1},  {0.19f, 108, 236, 1, 552},
                 {0.21f, 499, 236, 1, 552},  {0.23f, 109, 236, 390, 552},
                 {0.29f, 110, 215, 57, 20},  {0.31f, 108, 213, 61, 22}},
      .cycle_from = 5800,
      .cycle_to = 89300,
      .showcase = 0.62f,
  });

  s.push_back(Stage{
      .name = "Input",
      .title = "Idle loop, then a mouse click becomes a menu",
      .actor = "SDL event → SDLHAL queue → InputState process → primitives",
      .where = "main_sdl.cpp: push_input_word; asynchronousSignal; primInputWord (#95)",
      .steps = {"Idle, the image polls the mouse: primMousePt (primitive 90) → "
                "hal->get_cursor_location(), thousands of times a second.",
                "The yellow button goes down over the desktop. main() turns the SDL event into "
                "Blue Book input words: delta 0, x 800, y 850, delta 0, key-down 129.",
                "push_input_word queues each word and calls asynchronousSignal(19264) once per "
                "word; the interpreter signals the Semaphore at the next bytecode.",
                "The InputState process wakes and reads each word with primInputWord "
                "(primitive 95) → hal->next_input_word().",
                "ScreenController sees the yellow button and pops up the system menu: four "
                "copyBits and display_changed calls later it is on the screen."},
      .duration = 12.f,
      .accent = kSky,
      .active = {P::kInput, P::kMain, P::kHal, P::kSignals, P::kSensor, P::kPrims,
                 P::kControllers, P::kBitBltObj, P::kSdl},
      .flows = {{P::kSensor, P::kPrims, "#90 primMousePt", 0.02f, 0.20f, kSky},
                {P::kPrims, P::kHal, "get_cursor_location()", 0.05f, 0.22f, kSky},
                {P::kInput, P::kMain, "SDL_MOUSEBUTTONDOWN", 0.24f, 0.37f, kAmber, 0.5f},
                {P::kMain, P::kHal, "push_input_word ×5", 0.32f, 0.45f, kAmber},
                {P::kHal, P::kSignals, "asynchronousSignal(19264)", 0.40f, 0.54f, kAmber},
                {P::kSignals, P::kSensor, "wake InputState", 0.50f, 0.63f, kAmber},
                {P::kSensor, P::kPrims, "#95 primInputWord", 0.60f, 0.73f, kGreen},
                {P::kPrims, P::kHal, "next_input_word()", 0.62f, 0.75f, kGreen},
                {P::kControllers, P::kBitBltObj, "system menu", 0.74f, 0.86f, kRose},
                {P::kBitBltObj, P::kPrims, "#96 copyBits", 0.78f, 0.90f, kRose, 0.55f},
                {P::kPrims, P::kHal, "display_changed", 0.82f, 0.95f, kRose},
                {P::kHal, P::kSdl, "present", 0.86f, 0.98f, kRose}},
      .log = {T(0.03f, 89599, "prim", "#90 InputSensor>>primMousePt"),
              T(0.05f, 89599, "hal", "get_cursor_location() -> (0, 0)"),
              T(0.08f, 89964, "prim", "#88 Process>>suspend"),
              Note(0.14f, "idle: primMousePt x6149, Semaphore wait/signal x560 in 1.8M bytecodes"),
              T(0.26f, 1200000, "host", "mouse: button 129 down at (800, 850)"),
              T(0.34f, 1200000, "host", "input word 0x0000 queued, asynchronousSignal(19264)"),
              T(0.36f, 1200000, "host", "input word 0x1320 queued, asynchronousSignal(19264)"),
              T(0.38f, 1200000, "host", "input word 0x2352 queued, asynchronousSignal(19264)"),
              T(0.40f, 1200000, "host", "input word 0x0000 queued, asynchronousSignal(19264)"),
              T(0.42f, 1200000, "host", "input word 0x3081 queued, asynchronousSignal(19264)"),
              T(0.62f, 1200003, "hal", "next_input_word() -> 0x0000 (type 0, param 0)"),
              T(0.64f, 1200062, "hal", "next_input_word() -> 0x1320 (type 1, param 800)"),
              T(0.66f, 1200087, "hal", "next_input_word() -> 0x2352 (type 2, param 850)"),
              T(0.68f, 1200116, "hal", "next_input_word() -> 0x0000 (type 0, param 0)"),
              T(0.70f, 1200175, "hal", "next_input_word() -> 0x3081 (type 3, param 129)"),
              T(0.72f, 1200175, "prim", "#95 InputState>>primInputWord"),
              T(0.80f, 1202833, "prim", "#96 BitBlt>>copyBits"),
              T(0.83f, 1203066, "hal", "display_changed(x=734, y=666, w=136, h=195)"),
              T(0.85f, 1203298, "hal", "display_changed(x=733, y=665, w=136, h=195)"),
              T(0.87f, 1203603, "hal", "display_changed(x=734, y=666, w=133, h=192)"),
              T(0.89f, 1203804, "hal", "display_changed(x=734, y=842, w=133, h=16)"),
              T(0.93f, 1203884, "hal", "set_cursor_location(800, 850)")},
      .damage = {{0.83f, 734, 666, 136, 195},
                 {0.87f, 734, 666, 133, 192},
                 {0.89f, 734, 842, 133, 16}},
      .cycle_from = 89300,
      .cycle_to = 1203884,
      .pointer = true,
      .showcase = 0.9f,
  });
  return s;
}

std::vector<FileRegion> MakeFileMap() {
  return {
      {0, 512, "header (big-endian)", 0xFFFFB547, 1, 0.28f},
      {512, 591002, "object space: 295,501 words", 0xFFB18CFF, 1, 0.55f},
      {591514, 163840, "object table: 40,960 entries", 0xFF5CD6FF, 1, 0.42f},
  };
}

std::vector<KnownOop> MakeKnownOops() {
  return {
      {"nil", "2", 2, 0.45f},
      {"false", "4", 2, 0.48f},
      {"true", "6", 2, 0.51f},
      {"Processor", "8", 2, 0.62f},
      {"Smalltalk", "32288", 2, 0.66f},
      {"Metaclass", "by name", 2, 0.20f},
      {"SmallInteger", "by name", 2, 0.28f},
      {"CompiledMethod", "by name", 2, 0.32f},
      {"DisplayScreen", "by name", 2, 0.36f},
      {"Semaphore", "by name", 2, 0.40f},
      {"activeContext", "snapshotAs:thenQuit:", 3, 0.28f},
      {"input Semaphore", "19264", 4, 0.72f},
  };
}

std::vector<Screen> MakeScreens() {
  return {
      {4, 0.46f, 832},     {6, 0.05f, 8064},    {6, 0.15f, 8818},    {6, 0.23f, 9667},
      {6, 0.30f, 10700},   {6, 0.38f, 13000},   {6, 0.46f, 17000},   {6, 0.53f, 21400},
      {6, 0.61f, 31000},   {6, 0.68f, 35000},   {6, 0.75f, 41000},   {6, 0.81f, 45000},
      {6, 0.90f, 90000},   {7, 0.84f, 1900000},
  };
}

}  // namespace

const std::vector<Stage>& BootStages() {
  static const std::vector<Stage> stages = MakeStages();
  return stages;
}

const std::vector<FileRegion>& ImageFileMap() {
  static const std::vector<FileRegion> map = MakeFileMap();
  return map;
}

const std::vector<KnownOop>& KnownOops() {
  static const std::vector<KnownOop> oops = MakeKnownOops();
  return oops;
}

const std::vector<Screen>& Screens() {
  static const std::vector<Screen> screens = MakeScreens();
  return screens;
}

float TotalDuration() {
  float t = 0.f;
  for (const Stage& s : BootStages()) t += s.duration;
  return t;
}

Cursor CursorAt(float seconds) {
  const auto& stages = BootStages();
  float t = std::max(0.f, seconds);
  for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
    if (t < stages[i].duration) return {i, t / stages[i].duration};
    t -= stages[i].duration;
  }
  return {static_cast<int>(stages.size()) - 1, 1.f};
}

float SecondsAt(Cursor c) {
  const auto& stages = BootStages();
  float t = 0.f;
  for (int i = 0; i < c.stage && i < static_cast<int>(stages.size()); ++i) t += stages[i].duration;
  if (c.stage < static_cast<int>(stages.size())) t += c.progress * stages[c.stage].duration;
  return t;
}

std::vector<std::string> TraceUpTo(Cursor c) {
  std::vector<std::string> out;
  const auto& stages = BootStages();
  for (int i = 0; i <= c.stage && i < static_cast<int>(stages.size()); ++i) {
    for (const LogLine& l : stages[i].log) {
      if (i < c.stage || l.at <= c.progress) out.push_back(l.text);
    }
  }
  return out;
}

const Screen* ScreenAt(Cursor c) {
  const Screen* shown = nullptr;
  for (const Screen& s : Screens()) {
    if (s.stage < c.stage || (s.stage == c.stage && s.at <= c.progress)) shown = &s;
  }
  return shown;
}

long long CyclesAt(Cursor c) {
  const Stage& st = BootStages()[c.stage];
  const float p = std::clamp(c.progress, 0.f, 1.f);
  return st.cycle_from + static_cast<long long>((st.cycle_to - st.cycle_from) * p);
}

}  // namespace bootviz::scene
