// Geometric frame generation: capture the triangles the game draws.
//
// With hardware shaders off, Azahar runs the PICA vertex shaders on the CPU and hands
// every finished triangle (clip-space position + attributes) to its rasterizer. A small
// rasterizer that forwards everything to Azahar's own one sits in between and records
// those triangles, grouped by draw call, for each new picture the game shows - together
// with what the frame generator needs to know about each draw (which mesh and texture it
// is, how it is depth-tested, culled and blended). Nothing about the picture changes here.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace GeoCap
{
struct Vtx
{
    float pos[4];     // clip space, as the PICA vertex shader wrote it
    float tc0[2];
    float color[4];
};
// Draw::state
enum : uint32_t
{
    kBlend = 1u << 0,        // blended with what is already there (not a plain overwrite)
    kDepthTest = 1u << 1,
    kDepthFuncShift = 2,     // 3 bits: PICA compare function (0 never, 1 always, 2 ==, 3 !=, 4 <, 5 <=, 6 >, 7 >=)
    kDepthWrite = 1u << 5,
    kCullShift = 6,          // 2 bits: 0 / 3 = keep all, 1 = keep clockwise, 2 = keep counter-clockwise
    kAlphaTest = 1u << 8,
    kWBuffer = 1u << 9,      // depth from w instead of z
    kColorWrite = 1u << 10,
    kStencilTest = 1u << 11,
    kSrcShift = 12,          // 4 bits: source blend factor (colour)
    kDstShift = 16,          // 4 bits: destination blend factor (colour)
};
struct Draw
{
    uint32_t colorAddr = 0, depthAddr = 0;   // where it is drawn
    float vpW = 0, vpH = 0;                  // viewport size
    uint32_t tex0 = 0;                       // address of texture 0 (0 = none)
    uint32_t mesh = 0;                       // address of the vertex / index data: which mesh
    uint32_t state = 0;
    float depthScale = -1, depthOffset = 0;  // depth = z/w * scale + offset
    std::vector<Vtx> v;                      // 3 per triangle
};
struct Image
{
    std::vector<Draw> draws;
};

// Emulation thread, once per emulated frame: (re)puts the recording rasterizer in front of
// Azahar's (which is recreated on state loads and restarts).
void Install();
// Emulation thread, at the end of each emulated frame. newImage = the game delivered a new
// picture in it; the draws since the previous new picture belong to it (else nullptr).
std::shared_ptr<const Image> OnFrameEnd(bool newImage);
}
