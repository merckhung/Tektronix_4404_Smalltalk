# bootviz: how Tektronix 4404 Smalltalk-80 boots and calls the host

An animated 2D explanation of what happens when this repository's VM boots the
Tektronix 4404 `standardImage`: from `main()` and the snapshot loader to the
image resuming a process frozen in 1986, and how every call the image makes
(`TekSystemCall`, BitBlt, input) reaches the host through primitives, the
hardware abstraction layer (HAL) and SDL.

**Skia** draws the scene on the CPU. **Vulkan** renders an animated
circuit-board backdrop, composites the Skia layer over it, and then either
presents the result in a window or reads it back for screenshots. The program
is written in C++20 and built with Bazel (bzlmod). It follows the OluxOS
[bootviz](https://github.com/merckhung/oluxos/tree/main/tools/bootviz), whose
renderer and build layout it reuses.

![Stage 8: input](docs/screenshots/08_Input.png)

| | |
|---|---|
| ![Launch](docs/screenshots/01_Launch.png)<br>1. Launch | ![Load image](docs/screenshots/02_Load_image.png)<br>2. Load image |
| ![Calibrate](docs/screenshots/03_Calibrate.png)<br>3. Calibrate | ![Resume](docs/screenshots/04_Resume.png)<br>4. Resume |
| ![Display on](docs/screenshots/05_Display_on.png)<br>5. Display on | ![TekSystemCall](docs/screenshots/06_TekSystemCall.png)<br>6. TekSystemCall |
| ![Redraw](docs/screenshots/07_Redraw.png)<br>7. Redraw | ![Input](docs/screenshots/08_Input.png)<br>8. Input |

## What it shows

The diagram has three lanes: the **Smalltalk-80 image** (objects whose methods
run as bytecodes), the **C++ virtual machine** (`src/`), and the **host**
(`main_sdl.cpp`, Linux, SDL2). Each stage lights the blocks it uses and
animates the calls and data moving between them. The other panels show:

- the snapshot file's layout;
- the objects the VM has to know by oop;
- the Tektronix display bitmap at that moment, with the rectangles just
  reported to `display_changed()`;
- the recorded call trace.

| # | Stage | What happens | Where |
|---|-------|--------------|-------|
| 1 | Launch | `main()` creates `SDLHAL`, `PosixST80FileSystem` and the `Interpreter` | `main_sdl.cpp` |
| 2 | Load image | The loader reads the big-endian header, then 295,501 words of objects and a 40,960-entry object table | `src/objmemory.cpp` |
| 3 | Calibrate | `init()` finds classes by name (via `Metaclass`) and sets the known oops: nil 2, false 4, true 6, Processor 8, Smalltalk 32288 | `Interpreter::init` |
| 4 | Resume | The VM resumes `SystemDictionary>>snapshotAs:thenQuit:`, saved on 9 October 1986, and runs its "continue from a snapshot" branch | `standardSource` |
| 5 | Display on | `Display beDisplay` (primitive 102) calls `set_display_size(1024, 1024)`; `InputSensor install` registers the input semaphore (primitive 93) | `SystemDictionary>>install` |
| 6 | TekSystemCall | The image makes UniFLEX and display-driver calls (primitives 134/135: `cpint:to:`, `setpr:`, `getViewPort`...), which the VM stubs to answer true | `primitiveTekSystemCall` |
| 7 | Redraw | `ScheduledControllers restore` paints the desktop: 137 `copyBits`, 44 `scanCharacters`, 168 `display_changed` | `src/bitblt.cpp` |
| 8 | Input | Idle mouse polling (primitive 90), then a yellow-button click: input words, `asynchronousSignal`, `primInputWord` (95), and the system menu | `main_sdl.cpp`, `InputState` |

Every stage also has a numbered list of steps (the current step is
highlighted as the stage plays) and a pointer to the source.

### Where the data comes from

Nothing in the trace panel or the display panel is invented. `data/` is
recorded from the VM itself by `//:hal_trace`, a headless front end in the
repository root. It runs the image with a virtual clock of 4,000 bytecodes per
millisecond, so every run gives the same output. It logs:

- every primitive the image runs;
- every call to `IHardwareAbstractionLayer` and `IFileSystem`;
- each `TekSystemCall`, decoded (operation number, name and register
  arguments).

It also clicks the yellow button on the desktop and saves the 1024×1024
display at the moments the stages show.

```sh
tools/record_trace.sh     # rebuilds //:hal_trace, rewrites data/tektronix_trace.txt and data/screens/
```

The stage text (`src/scene/boot_script.cc`) quotes that trace, and the
Smalltalk method names come from `tektronix/system/standardSource`.

The interpreter exposes an optional primitive observer
(`Interpreter::setPrimitiveObserver`) for the recorder. It does nothing when
no observer is set.

## Build and run

Prerequisites (Ubuntu/Debian):

```sh
sudo apt install build-essential libvulkan1 mesa-vulkan-drivers libglfw3-dev \
                 glslang-tools fonts-dejavu-core git
# Bazel: bazelisk; .bazelversion pins Bazel 8.3.1
```

```sh
cd tools/bootviz
bazel run //:bootviz                                  # window, 1600x900
bazel run //:bootviz -- --speed=2 --stage=6           # start at TekSystemCall, twice as fast
bazel run //:bootviz -- --screenshot=$PWD/s.png --stage=7 --progress=0.5
tools/screenshots.sh                                  # regenerate docs/screenshots
bazel test //tests/...
```

The first build fetches Skia at a pinned commit and compiles about 600 of
its sources, which takes a few minutes. This directory is its own Bazel
module; the repository root's `.bazelignore` keeps the VM's build out of it.

| Key | Action |
|-----|--------|
| Space | Pause or resume |
| ← / → | Previous or next stage |
| 1–8 | Jump to a stage |
| + / − | Double or halve the speed |
| R, Home | Restart |
| Esc, Q | Quit |

`--headless` (implied by `--screenshot` and `--screenshots`) renders offscreen
with no display. Mesa's lavapipe software Vulkan driver is enough, which is
how the screenshots here were made. `--help` lists every flag.

### Restricted networks

Some networks allow `git clone` from GitHub but block archive downloads
(`github.com/.../archive/...`, `codeload.github.com`, the savannah mirrors)
or the Bazel Central Registry itself. On such a network, run

```sh
tools/git_modules.py
echo 'common --registry=https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main' >> user.bazelrc
```

once. `git_modules.py` clones vulkan_headers, stb, freetype and libpng at the
tag or commit their BCR entries were made from. It applies the registry's
patches and overlay files, checking their SHA-256, and then writes
`--override_module` lines to `user.bazelrc`, which is untracked and imported
by `.bazelrc`. The second line reads the registry itself from GitHub. If
`bazelisk` cannot download Bazel, set
`BAZELISK_BASE_URL=https://github.com/bazelbuild/bazel/releases/download`.

## Layout

```
MODULE.bazel         bzlmod deps (rules_cc, vulkan_headers, stb, freetype from the BCR);
                     Skia via git_repository + our BUILD overlay; host GLFW and GLSL compiler
bazel/               shader_tools.bzl, shaders.bzl (GLSL -> SPIR-V -> embedded C++),
                     system_libs.bzl (host GLFW)
data/                tektronix_trace.txt and screens/*.png recorded by //:hal_trace,
                     embedded into the binary
third_party/skia/    skia.BUILD overlay + generated source lists (CPU raster, FreeType fonts)
src/scene/           boot_script.* (the stages: text, flows, trace lines, screens),
                     scene.* (Skia drawing), screens.* (PNG decode), text.* (fonts)
src/render/          dlopen Vulkan loader, device/swapchain/offscreen context, compositor
                     (backdrop shader + Skia overlay), GLSL shaders
src/app/             window loop, keyboard, headless screenshots, flags
tests/               stage script consistency, Skia raster rendering of every stage
tools/               record_trace.sh, pbm2png.py, screenshots.sh, git_modules.py,
                     gen_skia_srcs.py, embed (SPIR-V and files -> C++)
```

## Licence

The files adapted from twn_election keep its Apache-2.0 licence
(`LICENSE.twn_election`). `NOTICE` lists them, and each one says so in its
header. Skia is BSD-3-Clause. The rest is under the repository's MIT licence.
