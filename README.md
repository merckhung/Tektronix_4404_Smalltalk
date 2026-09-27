# Tektronix 4404 Smalltalk-80

A "by the Blue Book" C++ implementation of the Smalltalk-80 virtual machine that
boots two historical virtual images to a fully interactive desktop:

- **Tektronix 4404** `standardImage` — Smalltalk-80 T2.1.3b (68000, big-endian).
  **This is the default image.**
- **Xerox** `snapshot.im` — the Blue Book Smalltalk-80 System Version 2
  (little-endian).

The interpreter, object memory, and BitBlt are based on Dan Banay's Smalltalk-80
implementation. This repository adds support for the Tektronix image format, a
[Bazel](https://bazel.build/) build with an SDL2 front end, and a working
mouse/keyboard input path for both images.

| Tektronix 4404 (default) | Xerox |
|---|---|
| ![Tektronix 4404 boot](images/tektronix_boot.png) | ![Xerox boot](images/xerox_boot.png) |

*Both screenshots are captured from this VM booting the shipped images.*

## Requirements

- [Bazel](https://bazel.build/) (or Bazelisk)
- SDL2 development libraries

```sh
# Debian/Ubuntu
sudo apt-get install libsdl2-dev
```

## Building

```sh
bazel build //:smalltalk_tek
bazel build //:hal_trace        # optional: headless boot recorder (see below)
```

A traditional `make` build is also available under `linux/` (see
[Other build systems](#other-build-systems)).

## Running

Run the binary **from the repository root** so it can find the virtual images
and the Smalltalk source/changes files.

```sh
# Tektronix 4404 image (default)
./bazel-bin/smalltalk_tek

# Xerox image
./bazel-bin/smalltalk_tek -xerox
```

> **Note:** the Tektronix image boots noticeably slower than the Xerox image —
> allow up to a minute to reach the desktop.

The virtual images ship with the repository:

| Image | Path |
|---|---|
| Tektronix 4404 (default) | `tektronix/standardImage` |
| Xerox | `files/snapshot.im` |

### Command-line options

| Option | Description |
|---|---|
| `-tek`, `--tektronix` | Boot the Tektronix 4404 image (default). |
| `-xerox`, `--xerox` | Boot the Xerox release image. |
| `-image <path>` | Load a snapshot from an explicit path. |
| `-three` | Use the three-button mouse mapping (see below). |
| `-screenshot <file>` | Periodically write a BMP screenshot to `<file>`. |
| `-exit-after <ms>` | Exit after `<ms>` milliseconds (useful for automation). |
| `-h`, `--help` | Show usage. |

## Using the mouse

Smalltalk-80 expects a three-button mouse: **Red** (select), **Yellow**
(menus / *do it*), and **Blue** (window operations such as close). By default a
two-button mouse is assumed:

| Mouse action | Smalltalk button |
|---|---|
| Left button | Red (select) |
| Right button | Yellow (menu / *do it*) |
| Ctrl + Left | Yellow (menu / *do it*) |
| Alt + Left | Blue (window operations) |

Pass `-three` for the native three-button mapping (Left = Red, Middle = Yellow,
Right = Blue).

Clicking the **Yellow** button over the desktop background raises the Smalltalk
system menu.

## Keyboard

Printable characters are sent as typed. Following Smalltalk-80 conventions,
assignment (`←`) is `Shift`-`-` and the up-arrow (`↑`) is `Shift`-`6`.

## How it works

The VM loads a virtual image (a snapshot of the Smalltalk object memory), then
resumes the suspended process and interprets bytecodes, rendering the display
bitmap to an SDL2 window and feeding mouse/keyboard events back into the image.

The two images differ in several low-level details that the loader and
interpreter handle per image type:

- **Object table layout** — the Xerox object table lives at the tail of the file
  (little-endian); the Tektronix table is stored after the object data
  (big-endian).
- **Byte order** — Tektronix words are big-endian and store byte 0 in the high
  half of a word; Xerox words are little-endian with byte 0 in the low half.
- **Known-object oops** — the special `nil`/`true`/`false`/scheduler pointers and
  the `SmallInteger` / `CompiledMethod` class oops used by method dispatch and
  garbage collection are set for each image.

## How it boots and calls the host (bootviz)

[`tools/bootviz`](tools/bootviz/README.md) is an animated walk through what
happens when this VM boots the Tektronix image, built with C++, Skia 2D,
Vulkan and Bazel. It starts at `main()` and the snapshot loader, shows the
image resuming the process that saved it in 1986, and follows the calls the
image makes (`TekSystemCall`, BitBlt, mouse input) through primitives, the HAL
and SDL. Each stage lights up the parts of the image, VM and host involved and
animates the calls between them. The trace and display panels replay a real
boot recorded by `//:hal_trace` (see below).

| | |
|---|---|
| ![1. Launch](tools/bootviz/docs/screenshots/01_Launch.png)<br>1. `main()` wires the host to the VM | ![2. Load image](tools/bootviz/docs/screenshots/02_Load_image.png)<br>2. The snapshot loader reads the 1986 image |
| ![3. Calibrate](tools/bootviz/docs/screenshots/03_Calibrate.png)<br>3. Finding the known objects | ![4. Resume](tools/bootviz/docs/screenshots/04_Resume.png)<br>4. Resuming `snapshotAs:thenQuit:` |
| ![5. Display on](tools/bootviz/docs/screenshots/05_Display_on.png)<br>5. `beDisplay` opens the SDL window | ![6. TekSystemCall](tools/bootviz/docs/screenshots/06_TekSystemCall.png)<br>6. UniFLEX calls, stubbed by the VM |
| ![7. Redraw](tools/bootviz/docs/screenshots/07_Redraw.png)<br>7. BitBlt paints the desktop | ![8. Input](tools/bootviz/docs/screenshots/08_Input.png)<br>8. A mouse click becomes a menu |

```sh
cd tools/bootviz
bazel run //:bootviz            # needs Vulkan (Mesa lavapipe is enough) and GLFW
```

### Recording the boot: `hal_trace`

`//:hal_trace` boots an image without a window and logs every primitive, HAL
and file-system call the image makes, with `TekSystemCall` operations
decoded. It can also click the mouse and save the display (as PBM) at given
bytecode counts. Time is virtual, so every run gives the same output.

```sh
bazel build //:hal_trace
./bazel-bin/hal_trace -cycles 2000000 -click 300 800 850 -display-at 90000 desktop.pbm
```

## Other build systems

The `linux/` directory contains a `make`-based build that produces the same VM:

```sh
cd linux && make
cd ..
./linux/Smalltalk            # Tektronix 4404 (default)
./linux/Smalltalk -xerox     # Xerox
```

The `osx/`, `windows/`, and `bsd/` directories contain the original per-platform
projects from the upstream implementation; the SDL2 front end (`main_sdl.cpp`)
and Bazel build are the supported path for booting both images.

## Credits

The core interpreter, object memory, and BitBlt are based on Dan Banay's
Smalltalk-80 implementation (MIT License). The Xerox virtual image originates
from Mario Wolczko's Smalltalk-80 archive; the Tektronix 4404 `standardImage` is
the Tektronix Smalltalk-80 T2.1.3b release.

## License

MIT — see [LICENSE](LICENSE).
