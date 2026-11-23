// Between the frontend (main.cpp) and the Vulkan presenter (vk_present_window.cpp, which
// replaces Azahar's own PresentWindow inside this program only).
//
// The core's Vulkan renderer draws both 3DS screens into a "Frame" image once per emulated
// frame and hands it to PresentWindow::Present. Here that image, together with what the
// frontend knows about it (did the game deliver a new picture, its triangles, where the
// screens are), goes to the presenter thread: it keeps the real images, makes the
// in-between ones (geometric frame generation), draws the menu on top and presents at the
// chosen rate (200 FPS by default) on its own swapchain.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GeoCap { struct Image; }

namespace Porte
{
struct FrameMeta
{
    bool changed = false;     // the game delivered a new picture in this emulated frame
    double time = 0;          // seconds (performance counter) when the core finished it
    double showAt = 0;        // > 0: an in-between picture of the interpolation, to be shown at this time (performance counter)
    std::shared_ptr<const GeoCap::Image> geo;   // its triangles (Geometric mode), else null
    float top[4] = {};                          // the top screen in the image: left, top, right, bottom (pixels, y down)
};

// one vertex of the menu / overlay (pixels, y down)
struct UiVert
{
    float x, y;       // position
    float u, v;       // font atlas
    float lx, ly;     // position inside the rectangle (rounded corners)
    float sw, sh;     // rectangle size
    float r, g, b, a;
    float mode;       // 0 = colour, 1 = text, 2 = rounded rectangle, 3 = controller symbol
    float radius;     // (mode 3: which symbol)
};

struct Hooks
{
    // emulation thread, from PresentWindow::Present (the core just finished a frame)
    FrameMeta (*onSwap)() = nullptr;
    // presenter thread: the menu / overlay for a w x h frame
    void (*buildUi)(int w, int h, std::vector<UiVert>& out) = nullptr;
    // presenter thread: a frame went to the screen (generated = an in-between one)
    void (*onPresented)(bool generated) = nullptr;
    // presenter thread, after each frame: a file name = save what was just shown there
    // (raw: int w, int h, then BGRA rows, top first); empty = nothing to save
    std::string (*shotWanted)() = nullptr;
};
extern Hooks gHooks;

// options the presenter reads every frame (written by the frontend)
extern std::atomic<bool> gGenOn;       // frame generation on (and not fast-forwarding)
extern std::atomic<int> gRate;         // frames per second when generating
extern std::atomic<bool> gVsync;       // no tearing (mailbox), else immediate
extern std::atomic<bool> gHold;        // menu open with "pause": keep showing the last image
extern std::atomic<bool> gMenuOpen;    // redraw at least 60 times a second
extern std::atomic<int> gFrameSkip;    // stats: frames the presenter dropped (more than one waiting)
extern std::atomic<int> gIngested;     // stats: new pictures the presenter got
// test (PORTE_LATENCIA): while true the presenter reads a small piece of each picture it shows and logs when
inline std::atomic<bool> gLatRec{ false };

// the menu font (R8), set before the core starts
extern const uint8_t* gAtlas;
extern int gAtlasW, gAtlasH;
// achievement pictures (RGBA8, kPicsW x kPicsW, cells of kPicCell), set before the core starts
constexpr int kPicsW = 512, kPicCell = 128;
extern const uint8_t* gPics;
}
