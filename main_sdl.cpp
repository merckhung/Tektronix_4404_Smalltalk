#include <SDL2/SDL.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include "interpreter.h"
#include "objmemory.h"
#include "posixfilesystem.h"

// -----------------------------------------------------------------------------
// Blue Book input event word encoding.
//
// The image's InputState>>run decodes each word as `type = word >> 12` and
// `param = word bitAnd: 4095`. A logical event (a mouse move, a click, a key
// press) is a short run of words, and the interpreter signals the input
// semaphore once per word pushed.
// -----------------------------------------------------------------------------
static constexpr uint16_t kEventDeltaTime = 0;  // elapsed ms since last event
static constexpr uint16_t kEventMouseX = 1;     // pointer x (display coords)
static constexpr uint16_t kEventMouseY = 2;     // pointer y (display coords)
static constexpr uint16_t kEventKeyDown = 3;    // key / button pressed
static constexpr uint16_t kEventKeyUp = 4;      // key / button released

static constexpr uint16_t EventWord(uint16_t type, int param) {
    return static_cast<uint16_t>((type << 12) | (param & 0x0FFF));
}

// Mouse buttons are keyset bit codes. InputSensor>>buttons is bitState bitAnd: 7
// where blue answers 1, yellow 2, red 4 -- so the codes are 128, 129, 130.
// Red is Smalltalk's primary (selection) button.
static constexpr int kBlueButton = 128;    // close windows
static constexpr int kYellowButton = 129;  // menu / doit
static constexpr int kRedButton = 130;     // selection

// Modifier key codes (InputState class>>initialize).
static constexpr int kLeftShiftKey = 136;
static constexpr int kRightShiftKey = 137;
static constexpr int kCtrlKey = 138;

class SDLHAL : public IHardwareAbstractionLayer {
public:
    SDLHAL(ImageType type, const char* imageName, bool threeButton)
        : window(nullptr), renderer(nullptr), texture(nullptr), width(0),
          height(0), interpreter(nullptr), inputSemaphore(0),
          imageType(type), imageName(imageName), threeButtonMouse(threeButton),
          timerSemaphore(0), timerActive(false), timerTargetMs(0),
          mouseX(0), mouseY(0), lastEventMs(0) {}
    ~SDLHAL() {
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void set_interpreter(Interpreter* interp) { interpreter = interp; }

    void error(const char *message) override {
        std::cerr << "VM ERROR: " << message << std::endl;
        // The diagnostics below themselves walk objects and can re-enter error();
        // guard so we report the first failure once instead of recursing.
        static bool inError = false;
        if (inError) return;
        inError = true;
        if (interpreter) {
            interpreter->printDisplayDiagnostics();
            interpreter->printStackTrace();
        }
        std::cerr << "Entering freeze loop to preserve display. Close window to exit." << std::endl;
        while (true) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    exit(1);
                }
            }
            SDL_Delay(100);
        }
    }

    const char* get_image_name() override { return imageName.c_str(); }
    void set_image_name(const char* name) override { imageName = name; }
    ImageType get_image_type() override { return imageType; }

    void set_input_semaphore(int semaphore) override {
        inputSemaphore = semaphore;
    }
    std::uint32_t get_smalltalk_epoch_time() override {
        // Seconds since 00:00 January 1, 1901.
        static const std::uint32_t kSecondsFrom1901To1970 = 2208988800u;
        return (std::uint32_t)time(nullptr) + kSecondsFrom1901To1970;
    }
    std::uint32_t get_msclock() override { return SDL_GetTicks(); }
    void signal_at(int semaphore, std::uint32_t msClockTime) override {
        // At most one outstanding request (G&R p.652). A new request supersedes
        // the previous one; semaphore 0 (image passed nil) cancels it.
        if (semaphore == 0) {
            timerActive = false;
            timerSemaphore = 0;
        } else {
            timerSemaphore = semaphore;
            timerTargetMs = msClockTime;
            timerActive = true;
        }
    }

    // Deliver a due timer semaphore. Called from the main loop.
    void check_timers() {
        if (!timerActive || !interpreter) return;
        // Use signed comparison so wrap-around of the 32-bit ms clock is handled.
        if ((std::int32_t)(SDL_GetTicks() - timerTargetMs) >= 0) {
            timerActive = false;
            interpreter->asynchronousSignal(timerSemaphore);
        }
    }

    void set_cursor_image(std::uint16_t *image) override {
        // TODO: Update SDL cursor image. The image still tracks the pointer via
        // primitiveMousePoint / get_cursor_location, so this is cosmetic.
    }
    void set_cursor_location(int x, int y) override {
        mouseX = x;
        mouseY = y;
    }
    void get_cursor_location(int *x, int *y) override {
        *x = mouseX;
        *y = mouseY;
    }
    void set_link_cursor(bool link) override {}

    bool set_display_size(int w, int h) override {
        if (width == w && height == h) return true;
        width = w; height = h;
        if (!window) {
            if (SDL_Init(SDL_INIT_VIDEO) < 0) {
                std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
                return false;
            }
            const char* title = imageType == ImageType::Tektronix
                ? "Smalltalk-80 (Tektronix 4404)"
                : "Smalltalk-80 (Xerox)";
            window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_SHOWN);
            if (!window) {
                std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
                return false;
            }
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
            if (!renderer) {
                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            }
            if (!renderer) {
                std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
                return false;
            }
#ifdef VM_DEBUG
            std::cerr << "SDL Window and Renderer created successfully: " << width << "x" << height << std::endl;
#endif
        } else {

            SDL_SetWindowSize(window, width, height);
        }
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width, height);
        return true;
    }

    void display_changed(int x, int y, int w, int h) override {
        if (!interpreter || !texture) return;
        int displayBits = interpreter->getDisplayBits(width, height);
        if (!displayBits) return;

        void* pixels;
        int pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) != 0) return;

        uint16_t* dst = (uint16_t*)pixels;
        int wordsPerRow = (width + 15) / 16;

        for (int row = 0; row < height; ++row) {
            int rowStart = row * wordsPerRow;
            for (int col = 0; col < width; ++col) {
                int wordIdx = rowStart + (col / 16);
                int bitIdx = 15 - (col % 16);
                uint16_t word = interpreter->fetchWord_ofDislayBits(wordIdx, displayBits);
                bool black = (word >> bitIdx) & 1;
                dst[row * (pitch/2) + col] = black ? 0x0000 : 0xFFFF;
            }
        }

        SDL_UnlockTexture(texture);
        render();
    }



    void render() {
        if (!renderer || !texture) return;
#ifdef VM_DEBUG
        static int render_count = 0;
        if (render_count++ % 100 == 0) {
            std::cerr << "SDL RenderPresent: count=" << render_count << std::endl;
        }
#endif
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    void save_screenshot(const char* filename) {
        if (!renderer || width == 0 || height == 0) return;
        SDL_Surface* saveSurface = SDL_CreateRGBSurface(0, width, height, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (!saveSurface) {
            std::cerr << "Failed to create surface for screenshot: " << SDL_GetError() << std::endl;
            return;
        }
        if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, saveSurface->pixels, saveSurface->pitch) != 0) {
            std::cerr << "Failed to read pixels for screenshot: " << SDL_GetError() << std::endl;
            SDL_FreeSurface(saveSurface);
            return;
        }
        if (SDL_SaveBMP(saveSurface, filename) != 0) {
            std::cerr << "Failed to save screenshot: " << SDL_GetError() << std::endl;
        }
#ifdef VM_DEBUG
        else {
            std::cerr << "Screenshot saved to " << filename << std::endl;
        }
#endif
        SDL_FreeSurface(saveSurface);
    }


    bool next_input_word(std::uint16_t *word) override {
        if (inputQueue.empty()) return false;
        *word = inputQueue.front();
        inputQueue.pop();
        return true;
    }

    void push_input_word(std::uint16_t word) {
        // Drop input until the image has installed its input semaphore. The
        // image reads exactly one word per semaphore signal, so any word queued
        // without a matching signal would permanently desync the queue from the
        // signal count and stall input (the image would always lag behind).
        if (inputSemaphore == 0 || !interpreter) return;
        inputQueue.push(word);
        // One signal per queued word.
        interpreter->asynchronousSignal(inputSemaphore);
    }

    // ---- Event helpers ------------------------------------------------------

    void push_delta_time() {
        std::uint32_t now = SDL_GetTicks();
        std::uint32_t delta = now - lastEventMs;
        lastEventMs = now;
        if (delta > 4095) delta = 4095;  // 12-bit parameter
        push_input_word(EventWord(kEventDeltaTime, (int)delta));
    }

    void push_mouse_position(int x, int y) {
        if (x < 0) x = 0; else if (x >= width && width > 0) x = width - 1;
        if (y < 0) y = 0; else if (y >= height && height > 0) y = height - 1;
        mouseX = x;
        mouseY = y;
        push_delta_time();
        push_input_word(EventWord(kEventMouseX, x));
        push_input_word(EventWord(kEventMouseY, y));
    }

    void push_button(int code, bool down) {
        push_delta_time();
        push_input_word(EventWord(down ? kEventKeyDown : kEventKeyUp, code));
    }

    // Translate an SDL mouse button (with modifiers) into a Smalltalk button.
    int map_mouse_button(Uint8 sdlButton) {
        SDL_Keymod mod = SDL_GetModState();
        if (threeButtonMouse) {
            switch (sdlButton) {
                case SDL_BUTTON_LEFT:   return kRedButton;
                case SDL_BUTTON_MIDDLE: return kYellowButton;
                case SDL_BUTTON_RIGHT:  return kBlueButton;
            }
            return 0;
        }
        // Two-button (default) mapping, matching the README:
        //   Left            -> Red (selection)
        //   Right           -> Yellow (doit/menu)
        //   Alt+Left        -> Blue (close)
        //   Ctrl+Left       -> Yellow (doit/menu)
        if (sdlButton == SDL_BUTTON_LEFT) {
            if (mod & KMOD_ALT) return kBlueButton;
            if (mod & KMOD_CTRL) return kYellowButton;
            return kRedButton;
        }
        if (sdlButton == SDL_BUTTON_RIGHT) return kYellowButton;
        if (sdlButton == SDL_BUTTON_MIDDLE) return kYellowButton;
        return 0;
    }

    void signal_quit() override { exit(0); }
    void exit_to_debugger() override {
        std::cout << "Exit to debugger requested" << std::endl;
    }

    int display_width() const { return width; }
    int display_height() const { return height; }

private:
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int width, height;
    Interpreter* interpreter;
    int inputSemaphore;
    std::queue<std::uint16_t> inputQueue;

    ImageType imageType;
    std::string imageName;
    bool threeButtonMouse;

    int timerSemaphore;
    bool timerActive;
    std::uint32_t timerTargetMs;

    int mouseX, mouseY;
    std::uint32_t lastEventMs;
};

// Map an SDL keysym to the code the image expects for a non-text key, or 0.
static int map_special_key(const SDL_Keysym& sym) {
    switch (sym.sym) {
        case SDLK_BACKSPACE: return 8;
        case SDLK_TAB:       return 9;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  return 13;
        case SDLK_ESCAPE:    return 27;
        case SDLK_DELETE:    return 127;
        case SDLK_LSHIFT:    return kLeftShiftKey;
        case SDLK_RSHIFT:    return kRightShiftKey;
        case SDLK_LCTRL:
        case SDLK_RCTRL:     return kCtrlKey;
        default: return 0;
    }
}

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  -tek | --tektronix   Boot the Tektronix 4404 image (default)\n"
              << "  -xerox | --xerox     Boot the Xerox release image\n"
              << "  -image <path>        Override the snapshot path to load\n"
              << "  -three               Use a three-button mouse mapping\n"
              << "  -screenshot <file>   Save a screenshot to <file> periodically\n"
              << "  -exit-after <ms>     Exit after <ms> milliseconds (for automation)\n"
              << "  -h | --help          Show this help\n";
}

int main(int argc, char** argv) {
    ImageType imageType = ImageType::Tektronix;
    std::string imageName = "tektronix/standardImage";
    bool imageExplicit = false;
    bool threeButton = false;
    std::string screenshotPath = "screenshot.bmp";
    long exitAfterMs = -1;
    // Optional scripted mouse click for automated input verification.
    long clickAtMs = -1;
    int clickX = 0, clickY = 0;
    Uint8 clickButton = SDL_BUTTON_RIGHT;  // maps to the yellow (menu) button

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-tek" || arg == "--tektronix" || arg == "-tektronix") {
            imageType = ImageType::Tektronix;
            if (!imageExplicit) imageName = "tektronix/standardImage";
        } else if (arg == "-xerox" || arg == "--xerox") {
            imageType = ImageType::Xerox;
            if (!imageExplicit) imageName = "files/snapshot.im";
        } else if (arg == "-image" && i + 1 < argc) {
            imageName = argv[++i];
            imageExplicit = true;
        } else if (arg == "-three" || arg == "--three") {
            threeButton = true;
        } else if (arg == "-screenshot" && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else if (arg == "-exit-after" && i + 1 < argc) {
            exitAfterMs = std::atol(argv[++i]);
        } else if (arg == "-clickat" && i + 3 < argc) {
            // -clickat <ms> <x> <y>: inject a yellow-button click for testing.
            clickAtMs = std::atol(argv[++i]);
            clickX = std::atoi(argv[++i]);
            clickY = std::atoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    SDLHAL hal(imageType, imageName.c_str(), threeButton);
    auto fs = std::make_unique<PosixST80FileSystem>(".");
    auto interpreter = std::make_unique<Interpreter>(&hal, fs.get());
    hal.set_interpreter(interpreter.get());

    if (!interpreter->init()) {
        std::cerr << "Interpreter init failed" << std::endl;
        return 1;
    }

    std::cout << "Starting Smalltalk-80 ("
              << (imageType == ImageType::Tektronix ? "Tektronix" : "Xerox")
              << " image: " << imageName << ")..." << std::endl;

    bool quit = false;
    uint32_t lastTick = SDL_GetTicks();
    uint32_t startTick = lastTick;

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    quit = true;
                    break;
                case SDL_MOUSEMOTION:
                    hal.push_mouse_position(event.motion.x, event.motion.y);
                    break;
                case SDL_MOUSEBUTTONDOWN: {
                    hal.push_mouse_position(event.button.x, event.button.y);
                    int code = hal.map_mouse_button(event.button.button);
                    if (code) hal.push_button(code, true);
                    break;
                }
                case SDL_MOUSEBUTTONUP: {
                    hal.push_mouse_position(event.button.x, event.button.y);
                    int code = hal.map_mouse_button(event.button.button);
                    if (code) hal.push_button(code, false);
                    break;
                }
                case SDL_TEXTINPUT: {
                    // Printable characters come through here with correct case.
                    for (const char* p = event.text.text; *p; ++p) {
                        unsigned char c = (unsigned char)*p;
                        if (c < 128) {
                            hal.push_button(c, true);
                            hal.push_button(c, false);
                        }
                    }
                    break;
                }
                case SDL_KEYDOWN: {
                    int code = map_special_key(event.key.keysym);
                    if (code) hal.push_button(code, true);
                    break;
                }
                case SDL_KEYUP: {
                    int code = map_special_key(event.key.keysym);
                    // Only the modifier keys need an explicit up event; the
                    // printable keys were delivered as down+up on TEXTINPUT.
                    if (code >= kLeftShiftKey) hal.push_button(code, false);
                    break;
                }
                default:
                    break;
            }
        }

        // Optional scripted click: push synthetic SDL events through the same
        // handler path a real click uses, to verify input end-to-end.
        if (clickAtMs >= 0 && (long)(SDL_GetTicks() - startTick) >= clickAtMs) {
            SDL_Event me;
            me.type = SDL_MOUSEMOTION;
            me.motion.x = clickX; me.motion.y = clickY;
            SDL_PushEvent(&me);
            me.type = SDL_MOUSEBUTTONDOWN;
            me.button.button = clickButton; me.button.x = clickX; me.button.y = clickY;
            SDL_PushEvent(&me);
            clickAtMs = -1;  // once
        }

        // Fire any due timer semaphore (Delay, cursor blink, scheduling).
        hal.check_timers();

        // Run some cycles
        for (int i=0; i<5000; i++) {
            interpreter->cycle();
        }

        // Periodically refresh in case no BitBlt triggered a redraw.
        uint32_t currentTick = SDL_GetTicks();
        if (currentTick - lastTick > 33) { // ~30 FPS
            hal.render();
            lastTick = currentTick;
        }

        static uint32_t lastScreenshotTick = 0;
        if (currentTick - lastScreenshotTick > 5000) { // every 5 seconds
            hal.save_screenshot(screenshotPath.c_str());
            lastScreenshotTick = currentTick;
        }

        if (exitAfterMs >= 0 && (long)(currentTick - startTick) >= exitAfterMs) {
            hal.render();
            hal.save_screenshot(screenshotPath.c_str());
            quit = true;
        }
    }

    return 0;
}
