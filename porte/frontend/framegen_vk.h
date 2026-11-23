// Geometric frame generation on Vulkan (presenter thread only).
//
// Pokémon X draws its world at 30 FPS. Every new picture the game delivers is copied into
// a small history together with its triangles (GeoCap). For the last two pictures A and B,
// the game's draws are paired (same mesh, same vertex count, least motion), and each
// presented frame draws those triangles again at the moment t in between (every vertex part
// of the way from A to B, the game's own depth test). Each pixel then knows exactly which
// point of the scene it shows and where that point is in A and in B; a surface map of each
// real image (depth + which draw) tells whether the point is visible there, and the colour
// is taken from the image(s) that see it. B is reached one game frame (33 ms) later.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include "porte_bridge.h"

VK_DEFINE_HANDLE(VmaAllocator)

namespace FGV
{
struct Setup
{
    vk::Device dev;
    vk::PhysicalDevice phys;
    VmaAllocator alloc = nullptr;
    vk::Format format = vk::Format::eB8G8R8A8Unorm;   // the core's frame images
    // records a command buffer, submits it and waits until the GPU is idle
    std::function<void(const std::function<void(vk::CommandBuffer)>&)> runNow;
    const uint8_t* atlas = nullptr;   // menu font (R8)
    int atlasW = 0, atlasH = 0;
    const uint8_t* pics = nullptr;    // achievement pictures (RGBA8, Porte::kPicsW square)
};

bool Init(const Setup& s);
void Shutdown();

uint32_t Width();
uint32_t Height();
// (re)creates every image for frames of w x h (uses runNow: the GPU is idle afterwards)
void Resize(uint32_t w, uint32_t h);

// A finished emulated frame (src in TransferSrcOptimal, the size given to Resize). Only
// frames with a new picture are copied. 'now' = when the presenter got it.
void Ingest(vk::CommandBuffer cmd, vk::Image src, const Porte::FrameMeta& m, double now);

// true when the next Render rewrites the pair's vertex buffer: the caller must first wait
// until no frame in flight uses it
bool PairPending();
// true when anything was ingested
bool Ready();
// the frame to show at 'now' (+ the menu) into the output image, left in TransferSrcOptimal.
// slot = which frame in flight (0..2): its descriptor set and menu buffer are rewritten.
void Render(vk::CommandBuffer cmd, double now, int slot, bool generate, bool hold,
            const std::vector<Porte::UiVert>& ui, bool& generated);
vk::Image Output();

// testing aids
bool EvalPending();
void RunEval();                                  // uses runNow
// test: a rectangle of the output (BGRA rows); false = not available
bool ReadOutputRect(int x, int y, int w, int h, std::vector<uint8_t>& bgra);
bool ReadOutput(std::vector<uint8_t>& bgra);     // uses runNow; the last rendered output, top row first
}
