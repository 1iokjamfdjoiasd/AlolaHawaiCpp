// Geometric frame generation on Vulkan - see framegen_vk.h.
//
// Per pair of real pictures A, B (only when a new picture arrives):
//   - pairing of the game's draws (mesh, vertex count, least motion), one vertex buffer with
//     every vertex's clip position in A and in B
//   - surface map of A and of B: depth + which paired draw is seen (x, y), whether a 3D
//     layer (shadow, effect) is visible there (z), whether interface is drawn there (w)
// Per presented frame (moment t):
//   - layer map at t: where the visible 3D layer point of each pixel is in A and in B
//   - the real pictures where nothing paired covers (top screen: A until halfway, then B)
//   - the paired solids at t: each pixel takes its colour from A and/or B where that exact
//     point of the scene is visible AND equally shadowed (else from the layer's own motion);
//     under the interface, the interface stays put when that is what A and B agree on
//   - the menu
#include "framegen_vk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#include <vk_mem_alloc.h>

#include "common/common_types.h"
#include "geocap.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace FGV
{
namespace
{
Setup S;
bool sInit = false;

// ---------------------------------------------------------------------------------------
// small Vulkan helpers
// ---------------------------------------------------------------------------------------
struct Img
{
    vk::Image image;
    VmaAllocation alloc = nullptr;
    vk::ImageView view;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};
struct Buf
{
    vk::Buffer buf;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    size_t size = 0;
};

Img MakeImage(uint32_t w, uint32_t h, vk::Format f, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect)
{
    Img r;
    const vk::ImageCreateInfo ci = {
        .imageType = vk::ImageType::e2D,
        .format = f,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage img = VK_NULL_HANDLE;
    VkImageCreateInfo raw = static_cast<VkImageCreateInfo>(ci);
    if (vmaCreateImage(S.alloc, &raw, &ai, &img, &r.alloc, nullptr) != VK_SUCCESS)
    {
        fprintf(stderr, "FGV: falha ao criar imagem %ux%u\n", w, h);
        return r;
    }
    r.image = vk::Image{ img };
    r.view = S.dev.createImageView({
        .image = r.image,
        .viewType = vk::ImageViewType::e2D,
        .format = f,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    });
    return r;
}
void FreeImage(Img& i)
{
    if (i.view) S.dev.destroyImageView(i.view);
    if (i.image) vmaDestroyImage(S.alloc, static_cast<VkImage>(i.image), i.alloc);
    i = Img{};
}
Buf MakeBuffer(size_t size, vk::BufferUsageFlags usage, bool readback)
{
    Buf b;
    const vk::BufferCreateInfo ci = { .size = size, .usage = usage, .sharingMode = vk::SharingMode::eExclusive };
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
               (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    VkBuffer raw = VK_NULL_HANDLE;
    VkBufferCreateInfo rci = static_cast<VkBufferCreateInfo>(ci);
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(S.alloc, &rci, &ai, &raw, &b.alloc, &info) != VK_SUCCESS)
    {
        fprintf(stderr, "FGV: falha ao criar buffer de %zu bytes\n", size);
        return b;
    }
    b.buf = vk::Buffer{ raw };
    b.map = info.pMappedData;
    b.size = size;
    return b;
}
void FreeBuffer(Buf& b)
{
    if (b.buf) vmaDestroyBuffer(S.alloc, static_cast<VkBuffer>(b.buf), b.alloc);
    b = Buf{};
}
// makes sure the buffer holds 'size' bytes (frees the old one: only when it is not in use)
void Ensure(Buf& b, size_t size, vk::BufferUsageFlags usage, bool readback)
{
    if (b.size >= size && b.buf) return;
    FreeBuffer(b);
    size_t n = 1 << 16;
    while (n < size) n *= 2;
    b = MakeBuffer(n, usage, readback);
}
void Flush(const Buf& b, size_t size) { vmaFlushAllocation(S.alloc, b.alloc, 0, size); }
void Invalidate(const Buf& b, size_t size) { vmaInvalidateAllocation(S.alloc, b.alloc, 0, size); }

void ToLayout(vk::CommandBuffer cmd, Img& i, vk::ImageLayout to, vk::PipelineStageFlags srcStage, vk::AccessFlags srcAccess,
              vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess, vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor)
{
    const vk::ImageMemoryBarrier b = {
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = i.layout,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = i.image,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    cmd.pipelineBarrier(srcStage, dstStage, {}, {}, {}, b);
    i.layout = to;
}

// ---------------------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------------------
// push constants shared by every pipeline (96 bytes)
struct PC
{
    float map[4];    // game clip -> window clip: X = map.x * w + map.y * y, Y = map.z * w + map.w * x
    float px[4];     // window pixels: x = px.x + px.y * (y / w), y = px.z + px.w * (x / w)
    float dep[4];    // x = depth scale, y = depth offset, z = w-buffer, w = depth function (PICA 0..7)
    float misc[4];   // x = t, y = what is drawn (0 solid, 1 3D layer, 2 interface), z = largest motion (pixels), w = depth tolerance
    float size[4];   // xy = image size, z = debug view
    float top[4];    // top screen in the image: left, top, right, bottom
    float box[4];    // where the 3D layers can be at this moment (pixels): outside, the layer map is not read
};
static_assert(sizeof(PC) == 112);

const char* kHeader = R"(#version 450
layout(push_constant) uniform PC { vec4 map; vec4 px; vec4 dep; vec4 misc; vec4 size; vec4 top; vec4 box; } pc;
vec2 At(vec4 c) { return vec2(pc.px.x + pc.px.y * c.y / c.w, pc.px.z + pc.px.w * c.x / c.w); }
float DepthOf(vec4 c)
{
    float q = c.z * pc.dep.x + pc.dep.y * c.w;
    return pc.dep.z > 0.5 ? q : q / c.w;
}
)";

const char* kFullVS = R"(
layout(location = 0) out vec2 vUV;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// the real images where no paired triangle covers: top screen from A until halfway, then B;
// the rest of the window (bottom screen, background) always from the newest image
const char* kBaseFS = R"(
layout(set = 0, binding = 0) uniform sampler2D uA;
layout(set = 0, binding = 1) uniform sampler2D uB;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
void main()
{
    vec2 p = gl_FragCoord.xy;
    bool top = p.x >= pc.top.x && p.x < pc.top.z && p.y >= pc.top.y && p.y < pc.top.w;
    ivec2 ip = ivec2(p);
    oColor = (top && pc.misc.x < 0.5) ? texelFetch(uA, ip, 0) : texelFetch(uB, ip, 0);
}
)";

const char* kGeoVS = R"(
layout(location = 0) in vec4 aC;    // the vertex one game frame before A (see BuildPair: on the curve)
layout(location = 1) in vec4 aA;    // the vertex in A: clip position as the game's vertex shader wrote it
layout(location = 2) in vec4 aB;    // the same vertex in B
layout(location = 3) in float aId;  // which paired draw (-1 = none)
layout(location = 0) out vec4 vA;
layout(location = 1) out vec4 vB;
layout(location = 2) out vec2 vQW;
layout(location = 3) flat out float vId;
void main()
{
    // the curve through C (t = -1), A (t = 0) and B (t = 1): a limb, a claw or a camera that
    // turns follows its arc, a start speeds up; at a steady speed it is the straight line
    float t = pc.misc.x;
    vec4 p = aA + t * 0.5 * (aB - aC) + t * t * 0.5 * (aB - 2.0 * aA + aC);
    // the 3DS top screen is drawn rotated (240 x 400); visible depth range: -w <= z <= 0
    gl_Position = vec4(pc.map.x * p.w + pc.map.y * p.y, pc.map.z * p.w + pc.map.w * p.x, -p.z, p.w);
    vA = aA;
    vB = aB;
    vQW = vec2(p.z * pc.dep.x + pc.dep.y * p.w, p.w);
    vId = aId;
}
)";

// surface map of a real image: x = depth, y = which paired draw, z = a 3D layer is visible,
// w = interface is drawn (each kind writes only its own channels, see the pipelines)
const char* kSurfFS = R"(
layout(location = 0) in vec4 vA;
layout(location = 1) in vec4 vB;
layout(location = 2) in vec2 vQW;
layout(location = 3) flat in float vId;
layout(location = 0) out vec4 oSurf;
void main()
{
    float d = clamp(pc.dep.z > 0.5 ? vQW.x : vQW.x / vQW.y, 0.0, 1.0);
    gl_FragDepth = d;
    oSurf = pc.misc.y < 0.5 ? vec4(d, vId, 0.0, 0.0) : vec4(0.0, 0.0, pc.misc.y > 2.5 ? 2.0 : 1.0, 1.0);   // z: 1 shadow, 2 light
}
)";

// Edge mask of a real picture: 1 where a nearer, different surface is within a few pixels.
// The game draws its outlines afterwards, over the whole screen, along the silhouettes: those
// pixels of the surface behind carry the outline of what is in front, so they are never used
// for the surface behind (the outline moves with the silhouette, not with what is behind it).
const char* kEdgeFS = R"(
layout(set = 0, binding = 2) uniform sampler2D uSA;
layout(set = 0, binding = 3) uniform sampler2D uSB;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oEdge;
vec4 S(ivec2 q) { return pc.misc.y > 0.5 ? texelFetch(uSB, q, 0) : texelFetch(uSA, q, 0); }
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lim = ivec2(pc.size.xy) - 1;
    vec4 s = S(p);
    int r = int(pc.size.w);
    float e = 0.0;
    for (int k = 0; k < 8 && e < 0.5; k++)
    {
        ivec2 dir = k == 0 ? ivec2(1, 0) : k == 1 ? ivec2(-1, 0) : k == 2 ? ivec2(0, 1) : k == 3 ? ivec2(0, -1)
                  : k == 4 ? ivec2(1, 1) : k == 5 ? ivec2(-1, -1) : k == 6 ? ivec2(1, -1) : ivec2(-1, 1);
        for (int j = 1; j <= r; j++)
        {
            vec4 n = S(clamp(p + dir * j, ivec2(0), lim));
            if (n.x < s.x - 1e-3 && abs(n.y - s.y) > 0.5) { e = 1.0; break; }
        }
    }
    oEdge = vec4(e);
}
)";

// layer map at moment t: where the visible 3D layer point is in A (xy) and in B (zw)
const char* kOvFS = R"(
layout(location = 0) in vec4 vA;
layout(location = 1) in vec4 vB;
layout(location = 2) in vec2 vQW;
layout(location = 3) flat in float vId;
layout(location = 0) out vec4 oOv;
void main()
{
    float d = clamp(pc.dep.z > 0.5 ? vQW.x : vQW.x / vQW.y, 0.0, 1.0);
    gl_FragDepth = d;
    if (vA.w < 1e-4 || vB.w < 1e-4) discard;
    vec2 pa = At(vA), pb = At(vB);
    if (distance(pa, pb) > pc.misc.z) discard;
    oOv = vec4(pa.x + (pc.misc.y > 2.5 ? 1e5 : 0.0), pa.y, pb);
}
)";

// the in-between picture
const char* kMidFS = R"(
layout(set = 0, binding = 0) uniform sampler2D uA;
layout(set = 0, binding = 1) uniform sampler2D uB;
layout(set = 0, binding = 2) uniform sampler2D uSA;
layout(set = 0, binding = 3) uniform sampler2D uSB;
layout(set = 0, binding = 5) uniform sampler2D uOv;
layout(set = 0, binding = 6) uniform sampler2D uEA;
layout(set = 0, binding = 7) uniform sampler2D uEB;
layout(location = 0) in vec4 vA;
layout(location = 1) in vec4 vB;
layout(location = 2) in vec2 vQW;
layout(location = 3) flat in float vId;
layout(location = 0) out vec4 oColor;

vec4 SA(vec2 p) { return texture(uA, p / pc.size.xy); }
vec4 SB(vec2 p) { return texture(uB, p / pc.size.xy); }
// The colour of point p (window pixels) in a real picture, filtered only over the pixels
// that show this same surface there (the surface map: the same draw at depth d, or another
// one at exactly that depth). A thin line keeps its contrast, and what is next to it never
// picks up its colour. ok = at least one pixel shows it; lay = a 3D layer is visible there;
// err = how far off the closest one is (for when none does).
vec4 Fetch(sampler2D img, sampler2D surf, sampler2D edge, vec2 p, float d, float tol, bool inside, out bool ok, out float lay, out float err)
{
    ok = false;
    lay = 0.0;
    err = 1e9;
    if (!inside) return vec4(0.0);
    vec2 q = p - 0.5;
    ivec2 i0 = ivec2(floor(q));
    vec2 f = q - floor(q);
    ivec2 lim = ivec2(pc.size.xy) - 1;
    vec4 acc = vec4(0.0);
    float ws = 0.0;
    for (int k = 0; k < 4; k++)
    {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 i = clamp(i0 + o, ivec2(0), lim);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) + 1e-4;
        vec4 s = texelFetch(surf, i, 0);
        bool same = abs(s.y - vId) < 0.5;
        float e = abs(s.x - d);
        err = min(err, e + (same ? 0.0 : 1.0));
        if (e <= (same ? tol : 0.5 * tol) && texelFetch(edge, i, 0).r < 0.5)
        {
            acc += texelFetch(img, i, 0) * w;
            lay += s.z * w;
            ws += w;
        }
    }
    ok = ws > 0.0;
    if (ok) { acc /= ws; lay /= ws; }
    return acc;
}
// average brightness of a picture around p (3x3 pixels)
float AvgLum(sampler2D s, vec2 p)
{
    float l = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            l += dot(texture(s, (p + vec2(x, y)) / pc.size.xy).rgb, vec3(0.299, 0.587, 0.114));
    return l / 9.0;
}
// how differently A around pa and B around pb look (0..1 per channel, 3x3 pixels)
float Differ(vec2 pa, vec2 pb)
{
    float s = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            vec2 o = vec2(x, y);
            s += dot(abs(SA(pa + o).rgb - SB(pb + o).rgb), vec3(1.0));
        }
    return s / 27.0;
}
void main()
{
    float d = pc.dep.z > 0.5 ? vQW.x : vQW.x / vQW.y;
    gl_FragDepth = clamp(d, 0.0, 1.0);
    if (vA.w < 1e-4 || vB.w < 1e-4) discard;
    vec2 pa = At(vA), pb = At(vB);
    if (distance(pa, pb) > pc.misc.z) discard;    // more than this in one game frame: not motion
    vec2 lo = pc.top.xy, hi = pc.top.zw;
    bool inA = all(greaterThanEqual(pa, lo)) && all(lessThan(pa, hi));
    bool inB = all(greaterThanEqual(pb, lo)) && all(lessThan(pb, hi));
    float dA = DepthOf(vA), dB = DepthOf(vB);
    // how much the depth of this surface changes from one pixel to the next: a point seen at
    // a pixel's centre is up to ~0.7 pixels away from the exact one
    float tolA = pc.misc.w + 1.5 * (abs(dFdx(dA)) + abs(dFdy(dA)));
    float tolB = pc.misc.w + 1.5 * (abs(dFdx(dB)) + abs(dFdy(dB)));
    bool visA, visB;
    float layA, layB, eA, eB;
    vec4 cA = Fetch(uA, uSA, uEA, pa, dA, tolA, inA, visA, layA, eA);
    vec4 cB = Fetch(uB, uSB, uEB, pb, dB, tolB, inB, visB, layB, eB);
    float t = pc.misc.x;

    // A shadow (or any 3D layer) over this point now? Then only samples equally shadowed in
    // their own picture are right; else only unshadowed ones.
    vec2 fp = gl_FragCoord.xy;
    vec4 ov = (fp.x >= pc.box.x && fp.y >= pc.box.y && fp.x < pc.box.z && fp.y < pc.box.w) ? texelFetch(uOv, ivec2(fp), 0) : vec4(-1e5);
    bool layer = ov.x > -1e4;
    bool light = ov.x > 5e4;   // light added on top (a headlight's beam), not a shadow
    if (light) ov.x -= 1e5;
    bool okA = visA && ((layA > 0.5) == layer), okB = visB && ((layB > 0.5) == layer);
    vec4 col;
    int how;   // debug: 0 both, 1 A, 2 B, 3 the layer's own motion, 4 neither
    if (light) { col = mix(SA(ov.xy), SB(ov.zw), t); how = 3; }   // the beam moves with its lamp, as bright as it is
    // a light that lit this point in one picture only (a headlight switching on / off): fade
    else if (!layer && visA && visB && (layA > 1.25 || layB > 1.25)) { col = mix(cA, cB, t); how = 0; }
    else if (okA && okB) { col = mix(cA, cB, t); how = 0; }
    else if (okA) { col = cA; how = 1; }
    else if (okB) { col = cB; how = 2; }
    else if (layer && (visA || visB))
    {
        // A shadow is over this point now, but it was not there in A or B (it moves fast, a
        // bike's): the surface's own picture (it moves the right way), as dark as the shadow
        // is here (from where the shadow was in A and B) - the ground never slides with it
        vec4 scene = visA && visB ? mix(cA, cB, t) : visA ? cA : cB;
        float ls = visA && visB ? mix(AvgLum(uA, pa), AvgLum(uB, pb), t) : visA ? AvgLum(uA, pa) : AvgLum(uB, pb);
        float ll = mix(AvgLum(uA, ov.xy), AvgLum(uB, ov.zw), t);
        col = vec4(scene.rgb * clamp(ll / max(ls, 1e-3), 0.2, 1.2), scene.a);
        how = 3;
    }
    else if (layer) { col = mix(SA(ov.xy), SB(ov.zw), t); how = 3; }
    else
    {
        // seen in neither (or shadowed differently in both): the one that comes closest
        if (visA && visB) col = mix(cA, cB, t);
        else if (visA) col = cA;
        else if (visB) col = cB;
        else col = eA <= eB ? SA(pa) : SB(pb);
        how = 4;
    }

    // Interface drawn here (text box, battle HUD, vignette): it stays put on the screen while
    // the scene may move under it. Whichever of the two A and B agree on wins: a box of text
    // stays sharp and still, a see-through vignette lets the scene move.
    float k = 0.0;
    vec2 p = gl_FragCoord.xy;
    if (texelFetch(uSB, ivec2(p), 0).w > 0.5 || texelFetch(uSA, ivec2(p), 0).w > 0.5)
    {
        vec4 still = t < 0.5 ? SA(p) : SB(p);   // nearest real picture, no fading
        float dL = Differ(p, p);
        float dS = (okA && okB) ? Differ(pa, pb) : 1.0;   // seen from one end only: cannot be checked
        // the scene only wins where its motion really explains A and B (a vignette over a
        // moving scene); when neither does - the text itself changed (letters appearing, a
        // new page, lines scrolling) - the interface stays: the nearest real picture, no ghost
        float wl = 1.0 / (dL * dL + 1e-4);
        float ws = 1.5 / (dS * dS + 1e-4) * smoothstep(0.06, 0.03, dS);
        k = wl / (wl + ws);
        col = mix(col, still, k);
    }
    oColor = col;
    if (pc.size.z > 0.5)
    {
        // green = both, red = A only, blue = B only, cyan = shadow's own motion, yellow = neither, magenta = interface
        vec3 c = how == 0 ? vec3(0, 1, 0) : how == 1 ? vec3(1, 0, 0) : how == 2 ? vec3(0, 0, 1) : how == 3 ? vec3(0, 1, 1) : vec3(1, 1, 0);
        c = mix(c, vec3(1, 0, 1), k);
        oColor = vec4(mix(col.rgb, c, 0.6), 1.0);
        // PORTE_DEBUGID: which paired draw (index in B) this pixel came from, as a number:
        // red = id % 256, green = id / 256 (blue 255 = none)
        if (pc.size.z > 1.5)
        {
            float id = max(vId, 0.0);
            oColor = vec4(mod(id, 256.0) / 255.0, floor(id / 256.0) / 255.0, vId < 0.0 ? 1.0 : 0.0, 1.0);
        }
    }
}
)";

// Signs over the screen (the area name banner): they slide with their own motion, but only
// where they are opaque - there A and B agree; where A and B disagree (the see-through parts,
// the scene moves behind them) the scene drawn below shows.
const char* kHudFS = R"(
layout(set = 0, binding = 0) uniform sampler2D uA;
layout(set = 0, binding = 1) uniform sampler2D uB;
layout(location = 0) in vec4 vA;
layout(location = 1) in vec4 vB;
layout(location = 2) in vec2 vQW;
layout(location = 3) flat in float vId;
layout(location = 0) out vec4 oColor;
vec4 SA(vec2 p) { return texture(uA, p / pc.size.xy); }
vec4 SB(vec2 p) { return texture(uB, p / pc.size.xy); }
void main()
{
    if (vA.w < 1e-4 || vB.w < 1e-4) discard;
    vec2 pa = At(vA), pb = At(vB);
    float d = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            vec2 o = vec2(x, y);
            d += dot(abs(SA(pa + o).rgb - SB(pb + o).rgb), vec3(1.0));
        }
    d /= 27.0;
    float a = smoothstep(0.10, 0.03, d);
    oColor = vec4(mix(SA(pa), SB(pb), pc.misc.x).rgb, a);
}
)";

const char* kUiVS = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec2 aLocal;
layout(location = 3) in vec2 aSize;
layout(location = 4) in vec4 aColor;
layout(location = 5) in vec2 aMode;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec2 vLocal;
layout(location = 2) flat out vec2 vSize;
layout(location = 3) out vec4 vColor;
layout(location = 4) flat out vec2 vMode;
void main()
{
    vUV = aUV;
    vLocal = aLocal;
    vSize = aSize;
    vColor = aColor;
    vMode = aMode;
    gl_Position = vec4(aPos / pc.size.xy * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kUiFS = R"(
layout(set = 0, binding = 4) uniform sampler2D uFont;
layout(set = 0, binding = 8) uniform sampler2D uPics;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec2 vLocal;
layout(location = 2) flat in vec2 vSize;
layout(location = 3) in vec4 vColor;
layout(location = 4) flat in vec2 vMode;
layout(location = 0) out vec4 oColor;
float Seg(vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}
// controller face-button symbols, outlines in a unit box (-1..1, y down): 0 cross, 1 circle, 2 square, 3 triangle
float PadShape(vec2 p, int s)
{
    const float th = 0.12;
    if (s == 0) return min(Seg(p, vec2(-0.55, -0.55), vec2(0.55, 0.55)), Seg(p, vec2(-0.55, 0.55), vec2(0.55, -0.55))) - th;
    if (s == 1) return abs(length(p) - 0.58) - th;
    if (s == 2) { vec2 q = abs(p) - vec2(0.5); return abs(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0)) - th; }
    if (s == 4)
    {
        // filled five-point star (achievement emblems)
        const vec2 k1 = vec2(0.809016994375, -0.587785252292), k2 = vec2(-k1.x, k1.y);
        vec2 q = vec2(abs(p.x), -p.y + 0.08);
        q -= 2.0 * max(dot(k1, q), 0.0) * k1;
        q -= 2.0 * max(dot(k2, q), 0.0) * k2;
        q.x = abs(q.x);
        q.y -= 0.95;
        vec2 ba = 0.42 * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
        float hh = clamp(dot(q, ba) / dot(ba, ba), 0.0, 0.95);
        return length(q - ba * hh) * sign(q.y * ba.x - q.x * ba.y);
    }
    const float k = 1.7320508, sc = 0.62;
    vec2 t = vec2(p.x, -p.y + 0.116) / sc;   // equilateral triangle, point up
    t.x = abs(t.x) - 1.0;
    t.y = t.y + 1.0 / k;
    if (t.x + k * t.y > 0.0) t = vec2(t.x - k * t.y, -k * t.x - t.y) / 2.0;
    t.x -= clamp(t.x, -2.0, 0.0);
    float tri = -length(t) * sign(t.y) * sc;
    return s == 5 ? tri : abs(tri) - th;   // 5 = filled
}
void main()
{
    int mode = int(vMode.x + 0.5);
    if (mode == 1) { oColor = vec4(vColor.rgb, vColor.a * texture(uFont, vUV).r); return; }
    if (mode == 4)
    {
        // achievement picture: vUV in the picture atlas, rounded corners; vMode.y = 1 grey (locked)
        vec4 c = texture(uPics, vUV);
        if (vMode.y > 0.5) c.rgb = vec3(dot(c.rgb, vec3(0.3, 0.59, 0.11))) * 0.55;
        vec2 h = vSize * 0.5;
        float r = min(h.x, h.y) * 0.35;
        vec2 q = abs(vLocal - h) - (h - vec2(r));
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        oColor = vec4(c.rgb, c.a * vColor.a * clamp(0.5 - d, 0.0, 1.0));
        return;
    }
    if (mode == 3)
    {
        float h = min(vSize.x, vSize.y) * 0.5;
        float d = PadShape((vLocal - vSize * 0.5) / h, int(vMode.y + 0.5)) * h;
        oColor = vec4(vColor.rgb, vColor.a * clamp(0.5 - d, 0.0, 1.0));
        return;
    }
    if (mode == 2)
    {
        vec2 h = vSize * 0.5;
        float r = vMode.y;
        vec2 q = abs(vLocal - h) - (h - vec2(r));
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        oColor = vec4(vColor.rgb, vColor.a * clamp(0.5 - d, 0.0, 1.0));
        return;
    }
    oColor = vColor;
}
)";

vk::ShaderModule Shader(const char* body, vk::ShaderStageFlagBits stage)
{
    const std::string code = std::string(kHeader) + body;
    vk::ShaderModule m = Vulkan::Compile(code, stage, S.dev);
    if (!m) fprintf(stderr, "FGV: shader nao compilou:\n%s\n", code.c_str());
    return m;
}

// ---------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------
constexpr vk::Format kSurfFormat = vk::Format::eR32G32B32A32Sfloat;
constexpr vk::Format kOvFormat = vk::Format::eR32G32B32A32Sfloat;
constexpr vk::Format kDepthFormat = vk::Format::eD32Sfloat;
constexpr int kSlots = 5;      // real images kept
constexpr int kInFlight = 3;   // descriptor sets / menu buffers (one per frame in flight)
constexpr int kEvalSet = kInFlight;
constexpr int kBindings = 9;
constexpr vk::Format kEdgeFormat = vk::Format::eR8Unorm;
const float kTol = 1e-4f;
const float kCutShift = 30.0f;   // native pixels in one game frame: more is a camera cut

uint32_t sW = 0, sH = 0;
vk::RenderPass sSurfRP, sOutRP, sOvRP, sEdgeRP;
vk::DescriptorSetLayout sSetLayout;
vk::PipelineLayout sLayout;
vk::DescriptorPool sPool;
vk::DescriptorSet sSets[kInFlight + 1];
vk::Sampler sLinear, sNearest;
vk::ShaderModule sFullVS, sBaseFS, sGeoVS, sSurfFS, sMidFS, sUiVS, sUiFS, sOvFS, sEdgeFS, sHudFS;
std::map<uint64_t, vk::Pipeline> sPipes;

Img sOut, sOutDepth, sSurf[2], sSurfDepth[2], sOv, sOvDepth, sEdge[2];
vk::Framebuffer sOutFB, sSurfFB[2], sOvFB, sEdgeFB[2];
Img sFont;
Img sPics;   // achievement pictures (binding 8)

struct Slot
{
    Img img;
    Porte::FrameMeta meta;
};
Slot sHist[kSlots];
int sA = -1, sB = -1, sC = -1, sD = -1;   // the last new pictures: D (oldest), C, A, B (newest)
double sPeriod = 1.0 / 30, sLastArrive = 0, sTransStart = 0;
bool sPairDirty = false;

// the pair's triangles
struct Batch
{
    int first, count;
    uint32_t state;
    float ds, doff;
    bool valid;
    bool ownDepth = false;   // see-through surface: our passes write its depth (the game did not)
};
constexpr int kVtxFloats = 13;
std::vector<float> sVB;   // per vertex: clip position one frame before A (4), in A (4), in B (4), paired draw (1)
struct Lists
{
    std::vector<Batch> mid;                  // paired solids, drawn at t
    std::vector<Batch> layers;               // paired 3D layers (shadows, effects), at t
    std::vector<Batch> lights;               // paired lights added on top (beams, glows), at t
    std::vector<Batch> hud;                  // paired signs over the screen (area names), at t
    std::vector<Batch> solid[2], layer[2], light[2], ui[2];   // what each picture shows (A, B), at its own moment
} sL;
bool sHasGeo = false;
int sBuildNo = 0;   // how many pairs were built (debug: PORTE_GEODUMPDRAWS=N lists the draws of the N-th)
bool sForceDump = false;   // debug: list the draws of the next pair built (PORTE_GEODUMPEVAL=N: the N-th evaluation)
int sPairA = -1, sPairB = -1;
float sBox[4] = { 0, 0, 0, 0 };   // window pixels covered by the paired 3D layers (A and B), empty = none
Buf sVBuf, sUiBuf[kInFlight], sRead;

// testing: PORTE_FGEVAL=N interpolates C -> B, compares with A (the real picture between them)
int sEvalLeft = 0, sEvalSkip = 0, sEvalDone = 0, sEvalGeo = 0;
bool sEvalPending = false;
double sEvalSum[4] = {}, sEvalBad[4] = {};

// ---------------------------------------------------------------------------------------
// pipelines
// ---------------------------------------------------------------------------------------
enum class Kind { Base, Ui, Surf, Mid, Ov, OvDepth, Edge, Hud };
// mask: colour channels written (bits r g b a)
vk::Pipeline Pipe(Kind kind, uint32_t func, bool write, uint32_t mask = 15)
{
    const uint64_t key = (uint64_t)kind | ((uint64_t)func << 8) | ((uint64_t)write << 12) | ((uint64_t)mask << 16);
    if (auto it = sPipes.find(key); it != sPipes.end()) return it->second;
    const bool geo = kind != Kind::Base && kind != Kind::Ui && kind != Kind::Edge;
    vk::ShaderModule vs = kind == Kind::Base || kind == Kind::Edge ? sFullVS : kind == Kind::Ui ? sUiVS : sGeoVS;
    vk::ShaderModule fs = kind == Kind::Base ? sBaseFS : kind == Kind::Ui ? sUiFS : kind == Kind::Mid ? sMidFS : kind == Kind::Ov ? sOvFS
                        : kind == Kind::Edge ? sEdgeFS : kind == Kind::Hud ? sHudFS : sSurfFS;
    vk::RenderPass rp = kind == Kind::Surf ? sSurfRP : (kind == Kind::Ov || kind == Kind::OvDepth) ? sOvRP : kind == Kind::Edge ? sEdgeRP : sOutRP;
    const vk::PipelineShaderStageCreateInfo stages[2] = {
        { .stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main" },
        { .stage = vk::ShaderStageFlagBits::eFragment, .module = fs, .pName = "main" },
    };
    const vk::VertexInputBindingDescription geoBind = { 0, kVtxFloats * sizeof(float), vk::VertexInputRate::eVertex };
    const vk::VertexInputAttributeDescription geoAttr[4] = {
        { 0, 0, vk::Format::eR32G32B32A32Sfloat, 0 },
        { 1, 0, vk::Format::eR32G32B32A32Sfloat, 16 },
        { 2, 0, vk::Format::eR32G32B32A32Sfloat, 32 },
        { 3, 0, vk::Format::eR32Sfloat, 48 },
    };
    const vk::VertexInputBindingDescription uiBind = { 0, sizeof(Porte::UiVert), vk::VertexInputRate::eVertex };
    const vk::VertexInputAttributeDescription uiAttr[6] = {
        { 0, 0, vk::Format::eR32G32Sfloat, 0 },
        { 1, 0, vk::Format::eR32G32Sfloat, 8 },
        { 2, 0, vk::Format::eR32G32Sfloat, 16 },
        { 3, 0, vk::Format::eR32G32Sfloat, 24 },
        { 4, 0, vk::Format::eR32G32B32A32Sfloat, 32 },
        { 5, 0, vk::Format::eR32G32Sfloat, 48 },
    };
    vk::PipelineVertexInputStateCreateInfo vi = {};
    if (geo)
        vi = { .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &geoBind,
               .vertexAttributeDescriptionCount = 4, .pVertexAttributeDescriptions = geoAttr };
    else if (kind == Kind::Ui)
        vi = { .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &uiBind,
               .vertexAttributeDescriptionCount = 6, .pVertexAttributeDescriptions = uiAttr };
    const vk::PipelineInputAssemblyStateCreateInfo ia = { .topology = vk::PrimitiveTopology::eTriangleList };
    const vk::PipelineViewportStateCreateInfo vp = { .viewportCount = 1, .scissorCount = 1 };
    const vk::PipelineRasterizationStateCreateInfo rs = {
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo ms = { .rasterizationSamples = vk::SampleCountFlagBits::e1 };
    static const vk::CompareOp ops[8] = { vk::CompareOp::eNever, vk::CompareOp::eAlways, vk::CompareOp::eEqual, vk::CompareOp::eNotEqual,
                                          vk::CompareOp::eLess, vk::CompareOp::eLessOrEqual, vk::CompareOp::eGreater, vk::CompareOp::eGreaterOrEqual };
    const vk::PipelineDepthStencilStateCreateInfo ds = {
        .depthTestEnable = geo,
        .depthWriteEnable = geo && write,
        .depthCompareOp = ops[func & 7],
    };
    vk::ColorComponentFlags cm{};
    if (mask & 1) cm |= vk::ColorComponentFlagBits::eR;
    if (mask & 2) cm |= vk::ColorComponentFlagBits::eG;
    if (mask & 4) cm |= vk::ColorComponentFlagBits::eB;
    if (mask & 8) cm |= vk::ColorComponentFlagBits::eA;
    const vk::PipelineColorBlendAttachmentState cb = {
        .blendEnable = kind == Kind::Ui || kind == Kind::Hud,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = cm,
    };
    const vk::PipelineColorBlendStateCreateInfo cbs = { .attachmentCount = 1, .pAttachments = &cb };
    const vk::DynamicState dyn[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    const vk::PipelineDynamicStateCreateInfo dys = { .dynamicStateCount = 2, .pDynamicStates = dyn };
    const vk::GraphicsPipelineCreateInfo ci = {
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cbs,
        .pDynamicState = &dys,
        .layout = sLayout,
        .renderPass = rp,
        .subpass = 0,
    };
    auto r = S.dev.createGraphicsPipeline({}, ci);
    if (r.result != vk::Result::eSuccess) fprintf(stderr, "FGV: pipeline falhou (%d)\n", (int)r.result);
    sPipes[key] = r.value;
    return r.value;
}

vk::RenderPass MakeRenderPass(vk::Format color, vk::ImageLayout finalColor)
{
    const vk::AttachmentDescription att[2] = {
        {
            .format = color,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
            .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = finalColor,
        },
        {
            .format = kDepthFormat,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eDontCare,
            .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
            .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        },
    };
    const vk::AttachmentReference cref = { 0, vk::ImageLayout::eColorAttachmentOptimal };
    const vk::AttachmentReference dref = { 1, vk::ImageLayout::eDepthStencilAttachmentOptimal };
    const vk::SubpassDescription sub = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &cref,
        .pDepthStencilAttachment = &dref,
    };
    const vk::PipelineStageFlags all = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests |
                                       vk::PipelineStageFlagBits::eLateFragmentTests;
    const vk::SubpassDependency deps[2] = {
        {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = all | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
            .dstStageMask = all,
            .srcAccessMask = {},
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                             vk::AccessFlagBits::eDepthStencilAttachmentRead,
        },
        {
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
        },
    };
    return S.dev.createRenderPass({
        .attachmentCount = 2,
        .pAttachments = att,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 2,
        .pDependencies = deps,
    });
}

vk::RenderPass MakeColorPass(vk::Format color, vk::ImageLayout finalColor)
{
    const vk::AttachmentDescription att = {
        .format = color,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = finalColor,
    };
    const vk::AttachmentReference cref = { 0, vk::ImageLayout::eColorAttachmentOptimal };
    const vk::SubpassDescription sub = { .pipelineBindPoint = vk::PipelineBindPoint::eGraphics, .colorAttachmentCount = 1, .pColorAttachments = &cref };
    const vk::SubpassDependency deps[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
          .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
          .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
          .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput, .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
          .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite, .dstAccessMask = vk::AccessFlagBits::eShaderRead },
    };
    return S.dev.createRenderPass({ .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
                                    .dependencyCount = 2, .pDependencies = deps });
}

// ---------------------------------------------------------------------------------------
// pairing the draws of A and B
// ---------------------------------------------------------------------------------------
enum { kSkip, kSolid, kOver };
bool IsTop(const GeoCap::Draw& d) { return std::fabs(d.vpW - 240.0f) < 1.0f && std::fabs(d.vpH - 400.0f) < 1.0f; }
// a sign over the screen: blended, no depth test (on top of everything), facing the camera
// (the same w at every vertex) - the area name banner and its letters
bool IsHud(const GeoCap::Draw& d)
{
    using namespace GeoCap;
    if ((d.state & kDepthTest) || !(d.state & kBlend) || d.v.empty()) return false;
    float w0 = d.v[0].pos[3], w1 = w0;
    for (const auto& v : d.v) { w0 = std::min(w0, v.pos[3]); w1 = std::max(w1, v.pos[3]); }
    return w0 > 1e-3f && (w1 - w0) <= 0.002f * w0;
}
// drawn straight onto the screen (no perspective): interface, text, 2D effects
bool IsFlat(const GeoCap::Draw& d)
{
    for (const auto& v : d.v)
        if (std::fabs(v.pos[3] - 1.0f) > 0.01f) return false;
    return !d.v.empty();
}
int Classify(const GeoCap::Draw& d)
{
    using namespace GeoCap;
    if (d.v.size() < 3 || !(d.state & kColorWrite)) return kSkip;
    const uint32_t src = (d.state >> kSrcShift) & 15, dst = (d.state >> kDstShift) & 15;
    const bool blend = (d.state & kBlend) != 0;
    if (blend && src == 0 && dst == 1) return kSkip;   // leaves the colour as it is (stencil / alpha-only passes)
    // A screen-wide pass that only scales what is already there (source factor zero, or the
    // destination colour: vignette, fade, tint) has no picture of its own: it is baked into
    // A and B and moves with the scene. Only flat draws that bring their own colour are interface.
    const bool ownColour = !blend || !(src == 0 || src == 4 || src == 5 || src == 8 || src == 9);
    if (IsFlat(d)) return ownColour ? kOver : kSkip;
    // A see-through 3D draw that only darkens / tints what is under it (shadows), or adds light
    // on top of it (destination factor one: lamp glows, light shafts), hides nothing behind
    // it: a layer. One that covers with a picture of its own (character outlines, hair,
    // decals) is a surface like the solids - it moves with its own triangles.
    if (blend && !(d.state & kDepthWrite) && (!ownColour || dst == 1)) return kOver;
    return kSolid;
}
uint64_t KeyOf(const GeoCap::Draw& d, int cls) { return ((uint64_t)d.mesh << 32) ^ ((uint64_t)d.v.size() << 4) ^ (uint64_t)cls; }
// how far a draw moved from A to B: median over a few of its vertices on (or near) the screen, native pixels
float Shift(const GeoCap::Draw& da, const GeoCap::Draw& db)
{
    float ds[32];
    int n = 0;
    const size_t cnt = db.v.size(), step = std::max<size_t>(1, cnt / 32);
    for (size_t i = 0; i < cnt && n < 32; i += step)
    {
        const float* pa = da.v[i].pos;
        const float* pb = db.v[i].pos;
        if (pa[3] < 1e-3f || pb[3] < 1e-3f) continue;
        const float ax = pa[0] / pa[3], ay = pa[1] / pa[3], bx = pb[0] / pb[3], by = pb[1] / pb[3];
        if (std::fabs(ax) > 2 || std::fabs(ay) > 2 || std::fabs(bx) > 2 || std::fabs(by) > 2) continue;
        const float dx = (bx - ax) * 120.0f, dy = (by - ay) * 200.0f;
        ds[n++] = std::sqrt(dx * dx + dy * dy);
    }
    if (n == 0) return -1.0f;
    std::nth_element(ds, ds + n / 2, ds + n);
    return ds[n / 2];
}
void MergeInto(std::vector<Batch>& list, const Batch& bt)
{
    if (!list.empty())
    {
        Batch& l = list.back();
        if (l.first + l.count == bt.first && l.state == bt.state && l.ds == bt.ds && l.doff == bt.doff && l.valid == bt.valid &&
            l.ownDepth == bt.ownDepth)
        {
            l.count += bt.count;
            return;
        }
    }
    list.push_back(bt);
}
bool HasTop(const GeoCap::Image* img)
{
    if (!img) return false;
    for (const auto& d : img->draws)
        if (IsTop(d)) return true;
    return false;
}

// Pairs the draws of X and Y that are the same thing: same mesh, vertex count and kind,
// the copies of one mesh by least motion. pairOfX[i] = its draw in Y (-1 = none).
void PairDraws(const GeoCap::Image& X, const GeoCap::Image& Y, uint32_t scene, std::vector<int>& clsX, std::vector<int>& clsY,
               std::vector<int>& pairOfX, std::vector<int>& pairOfY)
{
    const int nX = (int)X.draws.size(), nY = (int)Y.draws.size();
    clsX.assign(nX, kSkip);
    clsY.assign(nY, kSkip);
    pairOfX.assign(nX, -1);
    pairOfY.assign(nY, -1);
    std::unordered_map<uint64_t, std::pair<std::vector<int>, std::vector<int>>> groups;
    for (int i = 0; i < nX; i++)
    {
        const auto& d = X.draws[i];
        if (!IsTop(d) || d.colorAddr != scene) continue;
        clsX[i] = Classify(d);
        if (clsX[i] != kSkip) groups[KeyOf(d, clsX[i])].first.push_back(i);
    }
    for (int i = 0; i < nY; i++)
    {
        const auto& d = Y.draws[i];
        if (!IsTop(d) || d.colorAddr != scene) continue;
        clsY[i] = Classify(d);
        if (clsY[i] != kSkip) groups[KeyOf(d, clsY[i])].second.push_back(i);
    }
    const float maxShift = 80.0f;   // native pixels: more than this in one image is a cut, not motion
    struct Cand { float cost; int ix, iy; };
    std::vector<Cand> cands;
    for (auto& kv : groups)
    {
        auto& lx = kv.second.first;
        auto& ly = kv.second.second;
        if (lx.empty() || ly.empty()) continue;
        cands.clear();
        for (size_t x = 0; x < lx.size(); x++)
            for (size_t y = 0; y < ly.size(); y++)
            {
                const auto& dx = X.draws[lx[x]];
                const auto& dy = Y.draws[ly[y]];
                float c = Shift(dx, dy);
                if (c < 0) c = (lx.size() == 1 && ly.size() == 1) ? 0.0f : 1e6f;
                if (c > maxShift) continue;
                if (dx.tex0 != dy.tex0) c += 1.0f;
                c += 0.01f * std::fabs((float)x - (float)y);
                cands.push_back({ c, lx[x], ly[y] });
            }
        std::sort(cands.begin(), cands.end(), [](const Cand& p, const Cand& q) { return p.cost < q.cost; });
        for (const Cand& c : cands)
            if (pairOfX[c.ix] < 0 && pairOfY[c.iy] < 0) { pairOfX[c.ix] = c.iy; pairOfY[c.iy] = c.ix; }
    }
}
// median motion of the paired solids (native pixels), -1 = none to tell
float MedianShift(const GeoCap::Image& X, const GeoCap::Image& Y, const std::vector<int>& clsY, const std::vector<int>& pairOfY)
{
    std::vector<float> sh;
    for (int i = 0; i < (int)Y.draws.size(); i++)
        if (pairOfY[i] >= 0 && clsY[i] == kSolid)
        {
            const float d = Shift(X.draws[pairOfY[i]], Y.draws[i]);
            if (d >= 0) sh.push_back(d);
        }
    if (sh.empty()) return -1.0f;
    std::nth_element(sh.begin(), sh.begin() + sh.size() / 2, sh.end());
    return sh[sh.size() / 2];
}

// Fills sVB and the lists; false = nothing to interpolate with (a cut, no geometry).
// P = the picture before A (may be null), at time tp (A = 0, B = 1): with it each vertex
// follows the curve through P, A and B instead of the straight line.
bool BuildPair(const GeoCap::Image* P, const GeoCap::Image& A, const GeoCap::Image& B, float tp)
{
    static const bool dbg = getenv("PORTE_GEODEBUG") != nullptr;
    sVB.clear();
    sL = Lists{};
    std::vector<std::pair<uint32_t, int>> counts;
    for (const auto& d : B.draws)
    {
        if (!IsTop(d)) continue;
        auto it = std::find_if(counts.begin(), counts.end(), [&](auto& c) { return c.first == d.colorAddr; });
        if (it == counts.end()) counts.push_back({ d.colorAddr, 1 });
        else it->second++;
    }
    if (counts.empty()) return false;
    const uint32_t scene = std::max_element(counts.begin(), counts.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;

    ++sBuildNo;
    if (sForceDump || (getenv("PORTE_GEODUMPDRAWS") && sBuildNo == atoi(getenv("PORTE_GEODUMPDRAWS"))))
        for (size_t i = 0; i < B.draws.size(); i++)
        {
            const auto& d = B.draws[i];
            using namespace GeoCap;
            fprintf(stderr, "desenho %3zu: topo=%d alvo=%08X cena=%d vert=%6zu plano=%d cor=%d mistura=%d src=%u dst=%u zteste=%d zfunc=%u zescreve=%d stencil=%d alfa=%d cls=%d tex=%08X\n",
                    i, IsTop(d) ? 1 : 0, d.colorAddr, d.colorAddr == scene ? 1 : 0, d.v.size(), IsFlat(d) ? 1 : 0, (d.state & kColorWrite) ? 1 : 0,
                    (d.state & kBlend) ? 1 : 0, (d.state >> kSrcShift) & 15, (d.state >> kDstShift) & 15, (d.state & kDepthTest) ? 1 : 0,
                    (d.state >> kDepthFuncShift) & 7, (d.state & kDepthWrite) ? 1 : 0, (d.state & kStencilTest) ? 1 : 0,
                    (d.state & kAlphaTest) ? 1 : 0, Classify(d), d.tex0);
            // where it is on the top screen (400 x 240 native pixels) and its w range
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
            for (const auto& v : d.v)
            {
                w0 = std::min(w0, v.pos[3]); w1 = std::max(w1, v.pos[3]);
                if (v.pos[3] > 1e-4f)
                {
                    const float sx = (1.0f - v.pos[1] / v.pos[3]) * 200.0f, sy = (1.0f - v.pos[0] / v.pos[3]) * 120.0f;
                    x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
                }
            }
            fprintf(stderr, "    tela: x %.0f %.0f y %.0f %.0f w %.3f %.3f\n", x0, x1, y0, y1, w0, w1);
        }
    const int nA = (int)A.draws.size(), nB = (int)B.draws.size();
    std::vector<int> clsA, clsB, pairOfA, pairOfB;
    PairDraws(A, B, scene, clsA, clsB, pairOfA, pairOfB);
    // the same draws in the picture before A (unless that was a cut)
    std::vector<int> prevOfA(nA, -1);
    if (P)
    {
        std::vector<int> clsP, clsA2, pairOfP;
        PairDraws(*P, A, scene, clsP, clsA2, pairOfP, prevOfA);
        const float m = MedianShift(*P, A, clsA2, prevOfA);
        if (m < 0 || m > kCutShift) std::fill(prevOfA.begin(), prevOfA.end(), -1);
    }
    // the curve's point at t = -1 from the one at tp (Lagrange through tp, 0, 1)
    const float l0 = 2.0f / (tp * (tp - 1.0f)), l1 = 2.0f * (1.0f + tp) / tp, l2 = (1.0f + tp) / (1.0f - tp);

    // vertices: every draw that is used anywhere, once
    std::vector<Batch> batchOfA(nA, Batch{ 0, 0, 0, 0, 0, false, false });
    auto add = [&](const GeoCap::Draw* dp, const GeoCap::Draw* da, const GeoCap::Draw& db, float id) {
        const int first = (int)(sVB.size() / kVtxFloats);
        const size_t n = db.v.size() - db.v.size() % 3;
        sVB.resize(sVB.size() + n * kVtxFloats);
        float* out = sVB.data() + (size_t)first * kVtxFloats;
        const GeoCap::Draw& src = da ? *da : db;
        for (size_t i = 0; i < n; i++, out += kVtxFloats)
        {
            const float* a = src.v[i].pos;
            const float* b = db.v[i].pos;
            float c[4];
            for (int k = 0; k < 4; k++) c[k] = 2 * a[k] - b[k];   // straight line (no picture before A)
            if (dp && i < dp->v.size())
            {
                const float* q = dp->v[i].pos;
                float cc[4];
                for (int k = 0; k < 4; k++) cc[k] = l0 * q[k] + l1 * a[k] + l2 * b[k];
                // only a gentle curve: a bend much larger than the motion itself is a jump, not an arc
                const float bend = std::fabs(b[0] - 2 * a[0] + cc[0]) + std::fabs(b[1] - 2 * a[1] + cc[1]) + std::fabs(b[3] - 2 * a[3] + cc[3]);
                const float move = std::fabs(b[0] - a[0]) + std::fabs(b[1] - a[1]) + std::fabs(b[3] - a[3]) +
                                   std::fabs(a[0] - cc[0]) + std::fabs(a[1] - cc[1]) + std::fabs(a[3] - cc[3]);
                if (bend <= 1.5f * move + 1e-5f) std::copy(cc, cc + 4, c);
            }
            std::copy(c, c + 4, out);
            std::copy(a, a + 4, out + 4);
            std::copy(b, b + 4, out + 8);
            out[12] = id;
        }
        const bool seeThrough = (db.state & GeoCap::kBlend) && !(db.state & GeoCap::kDepthWrite);
        return Batch{ first, (int)n, db.state, db.depthScale, db.depthOffset, da != nullptr, seeThrough };
    };
    const float sceneShift = MedianShift(A, B, clsB, pairOfB);   // how far the scene moved (native pixels)
    int solidB = 0, solidPaired = 0;
    for (int i = 0; i < nB; i++)
    {
        if (clsB[i] == kSkip) continue;
        const auto& db = B.draws[i];
        const int ia = pairOfB[i];
        const GeoCap::Draw* dp = (ia >= 0 && prevOfA[ia] >= 0) ? &P->draws[prevOfA[ia]] : nullptr;
        const Batch bt = add(dp, ia >= 0 ? &A.draws[ia] : nullptr, db, ia >= 0 ? (float)i : -1.0f);
        if (ia >= 0) batchOfA[ia] = bt;
        if (clsB[i] == kSolid && IsHud(db))
        {
            // hides the scene behind it in B (surface map), drawn at t by its own pass
            MergeInto(sL.solid[1], bt);
            if (ia >= 0) MergeInto(sL.hud, bt);
            continue;
        }
        if (clsB[i] == kSolid)
        {
            solidB++;
            if (ia >= 0) { solidPaired++; MergeInto(sL.mid, bt); }
            MergeInto(sL.solid[1], bt);
        }
        else if (IsFlat(db)) MergeInto(sL.ui[1], bt);
        else
        {
            static const bool noLights = getenv("PORTE_NOLIGHTS") != nullptr;   // testing aid
            const bool add = !noLights && (db.state & GeoCap::kBlend) && ((db.state >> GeoCap::kDstShift) & 15) == 1 && !(db.state & GeoCap::kStencilTest);
            // a light carried by something that moves differently from the scene (a headlight's
            // beam on the bike the camera follows) moves with its own lamp; a glow fixed in the
            // world stays a layer like the shadows
            bool carried = false;
            if (add && ia >= 0)
            {
                const float sh = Shift(A.draws[ia], db);
                carried = sh >= 0 && sceneShift >= 0 && std::fabs(sh - sceneShift) > 1.5f;
            }
            if (ia >= 0) MergeInto(carried ? sL.lights : sL.layers, bt);
            MergeInto(add ? sL.light[1] : sL.layer[1], bt);
        }
    }
    for (int i = 0; i < nA; i++)
    {
        if (clsA[i] == kSkip) continue;
        const auto& da = A.draws[i];
        const Batch bt = pairOfA[i] >= 0 ? batchOfA[i] : add(nullptr, nullptr, da, -1.0f);
        if (clsA[i] == kSolid) MergeInto(sL.solid[0], bt);
        else if (IsFlat(da)) MergeInto(sL.ui[0], bt);
        else if ((da.state & GeoCap::kBlend) && ((da.state >> GeoCap::kDstShift) & 15) == 1 && !(da.state & GeoCap::kStencilTest)) MergeInto(sL.light[0], bt);
        else MergeInto(sL.layer[0], bt);
    }
    if (dbg)
        fprintf(stderr, "geo: A %d desenhos, B %d; solidos em B %d (casados %d), camadas 3D casadas %zu, interface %zu/%zu, %zu vertices\n", nA,
                nB, solidB, solidPaired, sL.layers.size(), sL.ui[0].size(), sL.ui[1].size(), sVB.size() / kVtxFloats);
    // too little in common: a cut to another scene
    if (solidB == 0 || solidPaired * 2 < solidB || sVB.empty()) return false;
    // the same models seen from another camera (battles cut between shots): the paired solids
    // jump much further than anything moves in one game frame
    const float med = MedianShift(A, B, clsB, pairOfB);
    if (dbg) fprintf(stderr, "geo: deslocamento mediano %.2f px nativos\n", med);
    if (med > kCutShift)
    {
        if (dbg) fprintf(stderr, "geo: corte de camera\n");
        return false;
    }
    return true;
}

// the window pixels the paired 3D layers can cover (in A, in B and in between)
void LayerBox(const float top[4])
{
    const float cx = 0.5f * (top[0] + top[2]), cy = 0.5f * (top[1] + top[3]), hw = 0.5f * (top[2] - top[0]), hh = 0.5f * (top[3] - top[1]);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    bool all = false;
    for (const auto* list : { &sL.layers, &sL.lights })
    for (const Batch& bt : *list)
        for (int v = bt.first; v < bt.first + bt.count && !all; v++)
            for (int e = 0; e < 2; e++)
            {
                const float* c = &sVB[(size_t)v * kVtxFloats + 4 + e * 4];
                if (c[3] < 1e-4f) { all = true; break; }   // reaches behind the camera: anywhere
                const float x = cx - hw * c[1] / c[3], y = cy - hh * c[0] / c[3];
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (sL.layers.empty() && sL.lights.empty()) { sBox[0] = sBox[1] = sBox[2] = sBox[3] = 0; return; }
    if (all) { std::copy(top, top + 4, sBox); return; }
    sBox[0] = std::max(top[0], std::floor(x0) - 2);
    sBox[1] = std::max(top[1], std::floor(y0) - 2);
    sBox[2] = std::min(top[2], std::ceil(x1) + 2);
    sBox[3] = std::min(top[3], std::ceil(y1) + 2);
    if (sBox[2] <= sBox[0] || sBox[3] <= sBox[1]) sBox[0] = sBox[1] = sBox[2] = sBox[3] = 0;
}

// push constants for the geometry of the pair (image size, top screen)
PC GeoPC(const float top[4], float t)
{
    PC pc = {};
    const float W = (float)sW, H = (float)sH;
    const float l = top[0], tp = top[1], r = top[2], b = top[3];
    const float cx = 0.5f * (l + r), cy = 0.5f * (tp + b), hw = 0.5f * (r - l), hh = 0.5f * (b - tp);
    pc.map[0] = 2 * cx / W - 1;
    pc.map[1] = -2 * hw / W;
    pc.map[2] = 2 * cy / H - 1;
    pc.map[3] = -2 * hh / H;
    pc.px[0] = cx;
    pc.px[1] = -hw;
    pc.px[2] = cy;
    pc.px[3] = -hh;
    pc.misc[0] = t;
    pc.misc[2] = 0.25f * (r - l);
    pc.misc[3] = kTol;
    pc.size[0] = W;
    pc.size[1] = H;
    std::copy(top, top + 4, pc.top);
    std::copy(sBox, sBox + 4, pc.box);
    return pc;
}
vk::Rect2D TopRect(const float top[4])
{
    const int x0 = std::clamp((int)std::floor(top[0]), 0, (int)sW), y0 = std::clamp((int)std::floor(top[1]), 0, (int)sH);
    const int x1 = std::clamp((int)std::ceil(top[2]), 0, (int)sW), y1 = std::clamp((int)std::ceil(top[3]), 0, (int)sH);
    return { { x0, y0 }, { (uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0) } };
}
uint32_t FuncOf(uint32_t state)
{
    return (state & GeoCap::kDepthTest) ? (state >> GeoCap::kDepthFuncShift) & 7 : 1;
}
// a layer lies on the surface it is drawn over: the strict tests become "or equal"
uint32_t LayerFunc(uint32_t state)
{
    const uint32_t f = FuncOf(state);
    return f == 4 ? 5 : f == 6 ? 7 : f;
}
// draws a list; what = 0 solid (game's depth test + write), 1 3D layer (test, no write), 2 interface
void DrawList(vk::CommandBuffer cmd, Kind kind, const std::vector<Batch>& list, int what, uint32_t mask, PC pc)
{
    pc.misc[1] = (float)what;
    vk::Pipeline bound;
    for (const Batch& bt : list)
    {
        const bool write = what == 0 && ((bt.state & GeoCap::kDepthWrite) != 0 || bt.ownDepth);
        const uint32_t func = what == 0 ? FuncOf(bt.state) : (what == 1 || what == 3) ? LayerFunc(bt.state) : 1;
        vk::Pipeline p = Pipe(kind, func, write, mask);
        if (p != bound) { cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, p); bound = p; }
        pc.dep[0] = bt.ds;
        pc.dep[1] = bt.doff;
        pc.dep[2] = (bt.state & GeoCap::kWBuffer) ? 1.0f : 0.0f;
        pc.dep[3] = (float)func;
        cmd.pushConstants(sLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PC), &pc);
        cmd.draw((uint32_t)bt.count, 1, (uint32_t)bt.first, 0);
    }
}
void SetViewport(vk::CommandBuffer cmd, vk::Rect2D scissor)
{
    const vk::Viewport vp = { 0, 0, (float)sW, (float)sH, 0, 1 };
    cmd.setViewport(0, vp);
    cmd.setScissor(0, scissor);
}

// both surface maps of the current pair
void RecordSurfaces(vk::CommandBuffer cmd)
{
    const vk::ClearValue clear[2] = { { .color = vk::ClearColorValue{ std::array<float, 4>{ 1.0f, -9.0f, 0, 0 } } },
                                      { .depthStencil = vk::ClearDepthStencilValue{ 1.0f, 0 } } };
    const vk::DeviceSize zero = 0;
    for (int i = 0; i < 2; i++)
    {
        cmd.beginRenderPass({ .renderPass = sSurfRP, .framebuffer = sSurfFB[i], .renderArea = { { 0, 0 }, { sW, sH } },
                              .clearValueCount = 2, .pClearValues = clear },
                            vk::SubpassContents::eInline);
        const Slot& s = sHist[i == 0 ? sPairA : sPairB];
        SetViewport(cmd, TopRect(sHist[sPairB].meta.top));
        cmd.bindVertexBuffers(0, sVBuf.buf, zero);
        const PC pc = GeoPC(s.meta.top, i == 0 ? 0.0f : 1.0f);
        DrawList(cmd, Kind::Surf, sL.solid[i], 0, 15, pc);   // depth, id (and clears the layer flags)
        DrawList(cmd, Kind::Surf, sL.layer[i], 1, 4, pc);    // z: a 3D layer is visible here
        DrawList(cmd, Kind::Surf, sL.light[i], 3, 4, pc);    // z = 2: a light
        DrawList(cmd, Kind::Surf, sL.ui[i], 2, 8, pc);       // w: interface
        cmd.endRenderPass();
        sSurf[i].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
}

// the edge masks of both surface maps (needs a descriptor set pointing at them)
void RecordEdges(vk::CommandBuffer cmd, vk::DescriptorSet set)
{
    PC pc = GeoPC(sHist[sPairB].meta.top, 0.0f);
    pc.size[3] = (float)std::max(1, (int)std::lround((pc.top[2] - pc.top[0]) / 400.0f * 0.75f));   // outline width, pixels
    for (int i = 0; i < 2; i++)
    {
        cmd.beginRenderPass({ .renderPass = sEdgeRP, .framebuffer = sEdgeFB[i], .renderArea = { { 0, 0 }, { sW, sH } } }, vk::SubpassContents::eInline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, sLayout, 0, set, {});
        SetViewport(cmd, { { 0, 0 }, { sW, sH } });
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipe(Kind::Edge, 1, false, 1));
        pc.misc[1] = (float)i;
        cmd.pushConstants(sLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PC), &pc);
        cmd.draw(3, 1, 0, 0);
        cmd.endRenderPass();
        sEdge[i].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
}

void WriteSet(vk::DescriptorSet set, int a, int b)
{
    vk::DescriptorImageInfo info[kBindings] = {
        { sLinear, sHist[a].img.view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sLinear, sHist[b].img.view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sNearest, sSurf[0].view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sNearest, sSurf[1].view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sLinear, sFont.view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sNearest, sOv.view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sNearest, sEdge[0].view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sNearest, sEdge[1].view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { sLinear, sPics.view, vk::ImageLayout::eShaderReadOnlyOptimal },
    };
    vk::WriteDescriptorSet w[kBindings];
    for (uint32_t i = 0; i < kBindings; i++)
        w[i] = { .dstSet = set, .dstBinding = i, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                 .pImageInfo = &info[i] };
    S.dev.updateDescriptorSets(w, {});
}

// the picture at t (+ menu): layer map, then real pictures + in-between triangles
void RecordOut(vk::CommandBuffer cmd, vk::DescriptorSet set, int b, float t, bool geo, const std::vector<Porte::UiVert>* ui, int uiSlot,
               bool debugView = false)
{
    const vk::DeviceSize zero = 0;
    const vk::Rect2D topRect = TopRect(sHist[b].meta.top);
    PC pc = GeoPC(sHist[b].meta.top, t);
    static const bool debugId = getenv("PORTE_DEBUGID") != nullptr;
    pc.size[2] = debugView ? (debugId ? 2.0f : 1.0f) : 0.0f;
    static const bool noLayers = getenv("PORTE_NOLAYERS") != nullptr;   // testing aid (cost)
    if (noLayers) pc.box[0] = pc.box[1] = pc.box[2] = pc.box[3] = 0;
    const vk::Rect2D box = { { (int32_t)pc.box[0], (int32_t)pc.box[1] },
                             { (uint32_t)std::max(0.0f, pc.box[2] - pc.box[0]), (uint32_t)std::max(0.0f, pc.box[3] - pc.box[1]) } };
    if (geo && box.extent.width > 0 && box.extent.height > 0)
    {
        // the visible 3D layers at this moment: the solids' depth first, then the layers
        const vk::ClearValue oc[2] = { { .color = vk::ClearColorValue{ std::array<float, 4>{ -1e5f, -1e5f, -1e5f, -1e5f } } },
                                       { .depthStencil = vk::ClearDepthStencilValue{ 1.0f, 0 } } };
        cmd.beginRenderPass({ .renderPass = sOvRP, .framebuffer = sOvFB, .renderArea = box, .clearValueCount = 2, .pClearValues = oc },
                            vk::SubpassContents::eInline);
        SetViewport(cmd, box);
        cmd.bindVertexBuffers(0, sVBuf.buf, zero);
        DrawList(cmd, Kind::OvDepth, sL.mid, 0, 0, pc);
        DrawList(cmd, Kind::Ov, sL.layers, 1, 15, pc);
        DrawList(cmd, Kind::Ov, sL.lights, 3, 15, pc);
        cmd.endRenderPass();
        sOv.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    const vk::ClearValue clear[2] = { { .color = vk::ClearColorValue{ std::array<float, 4>{ 0, 0, 0, 1 } } },
                                      { .depthStencil = vk::ClearDepthStencilValue{ 1.0f, 0 } } };
    cmd.beginRenderPass({ .renderPass = sOutRP, .framebuffer = sOutFB, .renderArea = { { 0, 0 }, { sW, sH } },
                          .clearValueCount = 2, .pClearValues = clear },
                        vk::SubpassContents::eInline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, sLayout, 0, set, {});
    SetViewport(cmd, { { 0, 0 }, { sW, sH } });
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipe(Kind::Base, 1, false));
    cmd.pushConstants(sLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PC), &pc);
    cmd.draw(3, 1, 0, 0);
    if (geo)
    {
        SetViewport(cmd, topRect);
        cmd.bindVertexBuffers(0, sVBuf.buf, zero);
        DrawList(cmd, Kind::Mid, sL.mid, 0, 15, pc);
        DrawList(cmd, Kind::Hud, sL.hud, 2, 15, pc);
    }
    if (ui && !ui->empty())
    {
        Buf& ub = sUiBuf[uiSlot];
        const size_t bytes = ui->size() * sizeof(Porte::UiVert);
        Ensure(ub, bytes, vk::BufferUsageFlagBits::eVertexBuffer, false);
        memcpy(ub.map, ui->data(), bytes);
        Flush(ub, bytes);
        SetViewport(cmd, { { 0, 0 }, { sW, sH } });
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipe(Kind::Ui, 1, false));
        cmd.pushConstants(sLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PC), &pc);
        cmd.bindVertexBuffers(0, ub.buf, zero);
        cmd.draw((uint32_t)ui->size(), 1, 0, 0);
    }
    cmd.endRenderPass();
    sOut.layout = vk::ImageLayout::eTransferSrcOptimal;
}

// makes (a, b) the current pair: pairing + vertex upload (the caller made sure nothing in flight uses the buffer)
// p = the picture before a (-1 = none) at time tp (a = 0, b = 1)
void UploadPair(int p, int a, int b, float tp)
{
    sPairA = a;
    sPairB = b;
    sHasGeo = false;
    const GeoCap::Image* prev = (p >= 0 && p != a && p != b) ? sHist[p].meta.geo.get() : nullptr;
    if (a >= 0 && b >= 0 && a != b && sHist[a].meta.geo && sHist[b].meta.geo)
        sHasGeo = BuildPair(prev, *sHist[a].meta.geo, *sHist[b].meta.geo, tp);
    if (sHasGeo) LayerBox(sHist[b].meta.top);
    if (sHasGeo)
    {
        const size_t bytes = sVB.size() * sizeof(float);
        Ensure(sVBuf, bytes, vk::BufferUsageFlagBits::eVertexBuffer, false);
        memcpy(sVBuf.map, sVB.data(), bytes);
        Flush(sVBuf, bytes);
    }
}

void DestroySized()
{
    for (vk::Framebuffer* f : { &sOutFB, &sSurfFB[0], &sSurfFB[1], &sOvFB, &sEdgeFB[0], &sEdgeFB[1] })
    {
        if (*f) S.dev.destroyFramebuffer(*f);
        *f = nullptr;
    }
    for (Img* i : { &sOut, &sOutDepth, &sSurf[0], &sSurf[1], &sSurfDepth[0], &sSurfDepth[1], &sOv, &sOvDepth, &sEdge[0], &sEdge[1] }) FreeImage(*i);
    for (auto& s : sHist) { FreeImage(s.img); s = Slot{}; }
    sA = sB = sC = sD = -1;
    sPairA = sPairB = -1;
    sHasGeo = false;
}
}   // namespace

// ---------------------------------------------------------------------------------------
bool Init(const Setup& s)
{
    S = s;
    sFullVS = Shader(kFullVS, vk::ShaderStageFlagBits::eVertex);
    sBaseFS = Shader(kBaseFS, vk::ShaderStageFlagBits::eFragment);
    sGeoVS = Shader(kGeoVS, vk::ShaderStageFlagBits::eVertex);
    sSurfFS = Shader(kSurfFS, vk::ShaderStageFlagBits::eFragment);
    sMidFS = Shader(kMidFS, vk::ShaderStageFlagBits::eFragment);
    sUiVS = Shader(kUiVS, vk::ShaderStageFlagBits::eVertex);
    sUiFS = Shader(kUiFS, vk::ShaderStageFlagBits::eFragment);
    sOvFS = Shader(kOvFS, vk::ShaderStageFlagBits::eFragment);
    sEdgeFS = Shader(kEdgeFS, vk::ShaderStageFlagBits::eFragment);
    sHudFS = Shader(kHudFS, vk::ShaderStageFlagBits::eFragment);
    if (!sFullVS || !sBaseFS || !sGeoVS || !sSurfFS || !sMidFS || !sUiVS || !sUiFS || !sOvFS || !sEdgeFS || !sHudFS) return false;

    vk::DescriptorSetLayoutBinding binds[kBindings];
    for (uint32_t i = 0; i < kBindings; i++)
        binds[i] = { .binding = i, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1,
                     .stageFlags = vk::ShaderStageFlagBits::eFragment };
    sSetLayout = S.dev.createDescriptorSetLayout({ .bindingCount = kBindings, .pBindings = binds });
    const vk::PushConstantRange pcr = { vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PC) };
    sLayout = S.dev.createPipelineLayout({ .setLayoutCount = 1, .pSetLayouts = &sSetLayout, .pushConstantRangeCount = 1,
                                           .pPushConstantRanges = &pcr });
    const vk::DescriptorPoolSize ps = { vk::DescriptorType::eCombinedImageSampler, kBindings * (kInFlight + 1) };
    sPool = S.dev.createDescriptorPool({ .maxSets = kInFlight + 1, .poolSizeCount = 1, .pPoolSizes = &ps });
    vk::DescriptorSetLayout layouts[kInFlight + 1];
    std::fill(std::begin(layouts), std::end(layouts), sSetLayout);
    const auto sets = S.dev.allocateDescriptorSets({ .descriptorPool = sPool, .descriptorSetCount = kInFlight + 1, .pSetLayouts = layouts });
    for (int i = 0; i <= kInFlight; i++) sSets[i] = sets[i];
    const auto sampler = [](vk::Filter f) {
        return S.dev.createSampler({ .magFilter = f, .minFilter = f, .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                     .addressModeU = vk::SamplerAddressMode::eClampToEdge, .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                     .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = 0.0f });
    };
    sLinear = sampler(vk::Filter::eLinear);
    sNearest = sampler(vk::Filter::eNearest);
    sSurfRP = MakeRenderPass(kSurfFormat, vk::ImageLayout::eShaderReadOnlyOptimal);
    sOutRP = MakeRenderPass(S.format, vk::ImageLayout::eTransferSrcOptimal);
    sOvRP = MakeRenderPass(kOvFormat, vk::ImageLayout::eShaderReadOnlyOptimal);
    sEdgeRP = MakeColorPass(kEdgeFormat, vk::ImageLayout::eShaderReadOnlyOptimal);

    // the menu font
    const int aw = std::max(1, S.atlasW), ah = std::max(1, S.atlasH);
    sFont = MakeImage((uint32_t)aw, (uint32_t)ah, vk::Format::eR8Unorm, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                      vk::ImageAspectFlagBits::eColor);
    Buf staging = MakeBuffer((size_t)aw * ah, vk::BufferUsageFlagBits::eTransferSrc, false);
    if (S.atlas) memcpy(staging.map, S.atlas, (size_t)aw * ah);
    else memset(staging.map, 0, (size_t)aw * ah);
    Flush(staging, (size_t)aw * ah);
    S.runNow([&](vk::CommandBuffer cmd) {
        ToLayout(cmd, sFont, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits::eTopOfPipe, {},
                 vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite);
        const vk::BufferImageCopy r = { .bufferOffset = 0, .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                                        .imageExtent = { (uint32_t)aw, (uint32_t)ah, 1 } };
        cmd.copyBufferToImage(staging.buf, sFont.image, vk::ImageLayout::eTransferDstOptimal, r);
        ToLayout(cmd, sFont, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite,
                 vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead);
    });
    FreeBuffer(staging);

    // the achievement pictures (empty = transparent when there are none)
    {
        const uint32_t pw = Porte::kPicsW;
        const size_t bytes = (size_t)pw * pw * 4;
        sPics = MakeImage(pw, pw, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                          vk::ImageAspectFlagBits::eColor);
        Buf st = MakeBuffer(bytes, vk::BufferUsageFlagBits::eTransferSrc, false);
        if (S.pics) memcpy(st.map, S.pics, bytes);
        else memset(st.map, 0, bytes);
        Flush(st, bytes);
        S.runNow([&](vk::CommandBuffer cmd) {
            ToLayout(cmd, sPics, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits::eTopOfPipe, {},
                     vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite);
            const vk::BufferImageCopy r = { .bufferOffset = 0, .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                                            .imageExtent = { pw, pw, 1 } };
            cmd.copyBufferToImage(st.buf, sPics.image, vk::ImageLayout::eTransferDstOptimal, r);
            ToLayout(cmd, sPics, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite,
                     vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead);
        });
        FreeBuffer(st);
    }

    if (const char* e = getenv("PORTE_FGEVAL")) sEvalLeft = atoi(e);
    sInit = true;
    return true;
}

void Shutdown()
{
    if (!sInit) return;
    S.runNow([](vk::CommandBuffer) {});
    DestroySized();
    FreeImage(sFont);
    FreeImage(sPics);
    FreeBuffer(sVBuf);
    FreeBuffer(sRead);
    for (auto& b : sUiBuf) FreeBuffer(b);
    for (auto& kv : sPipes) S.dev.destroyPipeline(kv.second);
    sPipes.clear();
    for (auto m : { sFullVS, sBaseFS, sGeoVS, sSurfFS, sMidFS, sUiVS, sUiFS, sOvFS, sEdgeFS, sHudFS }) S.dev.destroyShaderModule(m);
    S.dev.destroyRenderPass(sEdgeRP);
    S.dev.destroyRenderPass(sSurfRP);
    S.dev.destroyRenderPass(sOutRP);
    S.dev.destroyRenderPass(sOvRP);
    S.dev.destroySampler(sLinear);
    S.dev.destroySampler(sNearest);
    S.dev.destroyDescriptorPool(sPool);
    S.dev.destroyPipelineLayout(sLayout);
    S.dev.destroyDescriptorSetLayout(sSetLayout);
    // the core recreates its renderer (and this generator) on every state load: start over
    sW = sH = 0;
    sPairDirty = false;
    sEvalPending = false;
    sLastArrive = sTransStart = 0;
    sInit = false;
}

uint32_t Width() { return sW; }
uint32_t Height() { return sH; }

void Resize(uint32_t w, uint32_t h)
{
    S.runNow([](vk::CommandBuffer) {});
    DestroySized();
    sW = w;
    sH = h;
    if (!w || !h) return;
    using U = vk::ImageUsageFlagBits;
    sOut = MakeImage(w, h, S.format, U::eColorAttachment | U::eTransferSrc, vk::ImageAspectFlagBits::eColor);
    sOutDepth = MakeImage(w, h, kDepthFormat, U::eDepthStencilAttachment, vk::ImageAspectFlagBits::eDepth);
    const vk::ImageView ov[2] = { sOut.view, sOutDepth.view };
    sOutFB = S.dev.createFramebuffer({ .renderPass = sOutRP, .attachmentCount = 2, .pAttachments = ov, .width = w, .height = h, .layers = 1 });
    for (int i = 0; i < 2; i++)
    {
        sSurf[i] = MakeImage(w, h, kSurfFormat, U::eColorAttachment | U::eSampled, vk::ImageAspectFlagBits::eColor);
        sSurfDepth[i] = MakeImage(w, h, kDepthFormat, U::eDepthStencilAttachment, vk::ImageAspectFlagBits::eDepth);
        const vk::ImageView v[2] = { sSurf[i].view, sSurfDepth[i].view };
        sSurfFB[i] = S.dev.createFramebuffer({ .renderPass = sSurfRP, .attachmentCount = 2, .pAttachments = v, .width = w, .height = h, .layers = 1 });
    }
    sOv = MakeImage(w, h, kOvFormat, U::eColorAttachment | U::eSampled, vk::ImageAspectFlagBits::eColor);
    sOvDepth = MakeImage(w, h, kDepthFormat, U::eDepthStencilAttachment, vk::ImageAspectFlagBits::eDepth);
    const vk::ImageView lv[2] = { sOv.view, sOvDepth.view };
    sOvFB = S.dev.createFramebuffer({ .renderPass = sOvRP, .attachmentCount = 2, .pAttachments = lv, .width = w, .height = h, .layers = 1 });
    for (int i = 0; i < 2; i++)
    {
        sEdge[i] = MakeImage(w, h, kEdgeFormat, U::eColorAttachment | U::eSampled, vk::ImageAspectFlagBits::eColor);
        sEdgeFB[i] = S.dev.createFramebuffer({ .renderPass = sEdgeRP, .attachmentCount = 1, .pAttachments = &sEdge[i].view, .width = w, .height = h, .layers = 1 });
    }
    for (auto& s : sHist)
        s.img = MakeImage(w, h, S.format, U::eTransferDst | U::eSampled | U::eTransferSrc, vk::ImageAspectFlagBits::eColor);
    // everything a descriptor set may point at starts out readable
    S.runNow([](vk::CommandBuffer cmd) {
        for (Img* i : { &sSurf[0], &sSurf[1], &sOv, &sEdge[0], &sEdge[1] })
            ToLayout(cmd, *i, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eTopOfPipe, {},
                     vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead);
        for (auto& s : sHist)
            ToLayout(cmd, s.img, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eTopOfPipe, {},
                     vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead);
    });
}

void Ingest(vk::CommandBuffer cmd, vk::Image src, const Porte::FrameMeta& m, double now)
{
    if (!m.changed || !sW) return;
    int s = 0;
    while (s == sA || s == sB || s == sC || s == sD) s++;
    Slot& slot = sHist[s];
    ToLayout(cmd, slot.img, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits::eAllCommands, {},
             vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite);
    const vk::ImageMemoryBarrier srcBarrier = {
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src,
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    };
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, srcBarrier);
    const vk::ImageCopy region = {
        .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .extent = { sW, sH, 1 },
    };
    cmd.copyImage(src, vk::ImageLayout::eTransferSrcOptimal, slot.img.image, vk::ImageLayout::eTransferDstOptimal, region);
    ToLayout(cmd, slot.img, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite,
             vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead);
    slot.meta = m;
    sD = sC;
    sC = sA;
    sA = sB;
    sB = s;
    // the rhythm of new pictures: each transition lasts about as long as the time between them
    if (sLastArrive > 0)
    {
        const double dt = now - sLastArrive;
        if (dt > 0 && dt < 0.2) sPeriod = 0.85 * sPeriod + 0.15 * std::clamp(dt, 1.0 / 120, 0.1);
    }
    sLastArrive = now;
    sTransStart = now;
    sPairDirty = true;
    static const auto processStart = std::chrono::steady_clock::now();
    static const double evalAt = getenv("PORTE_FGEVALAT") ? atof(getenv("PORTE_FGEVALAT")) : 0.0;   // seconds after start
    const bool evalTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - processStart).count() >= evalAt;
    if (sEvalLeft > 0 && evalTime && sC >= 0 && HasTop(sHist[sC].meta.geo.get()) && HasTop(sHist[sA].meta.geo.get()) &&
        HasTop(sHist[sB].meta.geo.get()))
    {
        static const int every = std::max(2, getenv("PORTE_FGEVALEVERY") ? atoi(getenv("PORTE_FGEVALEVERY")) : 2);
        if (++sEvalSkip % every == 0) sEvalPending = true;
    }
}

bool PairPending() { return sPairDirty; }
bool Ready() { return sB >= 0 && sW > 0; }
vk::Image Output() { return sOut.image; }

void Render(vk::CommandBuffer cmd, double now, int slot, bool generate, bool hold, const std::vector<Porte::UiVert>& ui, bool& generated)
{
    generated = false;
    if (!Ready()) return;
    bool edges = false;
    if (sPairDirty)
    {
        sPairDirty = false;
        UploadPair(sC, sA, sB, -1.0f);
        if (sHasGeo) { RecordSurfaces(cmd); edges = true; }
    }
    const int a = sPairA >= 0 ? sPairA : sB, b = sPairB >= 0 ? sPairB : sB;
    float t = 1.0f;
    if (generate && !hold && sHasGeo) t = (float)std::clamp((now - sTransStart) / sPeriod, 0.0, 1.0);
    const bool geo = sHasGeo && t > 0.0f && t < 1.0f;
    generated = geo;
    WriteSet(sSets[slot], a, b);
    if (edges) RecordEdges(cmd, sSets[slot]);
    RecordOut(cmd, sSets[slot], b, sHasGeo ? t : 1.0f, geo, &ui, slot);
}

bool EvalPending() { return sEvalPending && sEvalLeft > 0; }

void RunEval()
{
    sEvalPending = false;
    const int i0 = sC, i1 = sA, i2 = sB;
    if (i0 < 0 || i1 < 0 || i2 < 0) return;
    S.runNow([](vk::CommandBuffer) {});   // nothing in flight uses the pair's buffer now
    sForceDump = getenv("PORTE_GEODUMPEVAL") && atoi(getenv("PORTE_GEODUMPEVAL")) == sEvalDone + 1;
    UploadPair(sD, i0, i2, -0.5f);   // D is one game frame before C; C -> B spans two
    sForceDump = false;
    const bool geo = sHasGeo;
    // the game draws at a steady 30 FPS of game time: the middle picture is exactly halfway
    const float t = 0.5f;
    const vk::Rect2D r = TopRect(sHist[i2].meta.top);
    const uint32_t w = r.extent.width, h = r.extent.height;
    if (!w || !h) { sPairDirty = true; return; }
    const size_t one = (size_t)w * h * 4;
    Ensure(sRead, one * 4, vk::BufferUsageFlagBits::eTransferDst, true);
    const vk::BufferImageCopy reg = { .bufferOffset = 0, .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                                      .imageOffset = { r.offset.x, r.offset.y, 0 }, .imageExtent = { w, h, 1 } };
    S.runNow([&](vk::CommandBuffer cmd) {
        if (geo) RecordSurfaces(cmd);
        WriteSet(sSets[kEvalSet], i0, i2);
        if (geo) RecordEdges(cmd, sSets[kEvalSet]);
        RecordOut(cmd, sSets[kEvalSet], i2, geo ? t : 1.0f, geo, nullptr, 0);
        cmd.copyImageToBuffer(sOut.image, vk::ImageLayout::eTransferSrcOptimal, sRead.buf, reg);
        const int src[3] = { i1, i0, i2 };   // the real middle picture, A, B
        for (int k = 0; k < 3; k++)
        {
            Img& im = sHist[src[k]].img;
            ToLayout(cmd, im, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead,
                     vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead);
            vk::BufferImageCopy rk = reg;
            rk.bufferOffset = one * (k + 1);
            cmd.copyImageToBuffer(im.image, vk::ImageLayout::eTransferSrcOptimal, sRead.buf, rk);
            ToLayout(cmd, im, vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead,
                     vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eShaderRead);
        }
    });
    Invalidate(sRead, one * 4);
    const uint8_t* px = (const uint8_t*)sRead.map;
    static const int dumpN = getenv("PORTE_FGEVALDUMP") ? atoi(getenv("PORTE_FGEVALDUMP")) : 0;
    static const std::string dumpDir = getenv("PORTE_DATA") ? std::string(getenv("PORTE_DATA")) + "\\" : std::string();
    const auto dump = [&](const char* what, const uint8_t* data) {
        char name[64];
        snprintf(name, sizeof(name), "eval_%02d_%s.raw", sEvalDone + 1, what);
        if (FILE* f = fopen((dumpDir + name).c_str(), "wb"))
        {
            const int hdr[2] = { (int)w, (int)h };
            fwrite(hdr, sizeof(hdr), 1, f);
            fwrite(data, 1, one, f);
            fclose(f);
        }
    };
    const uint8_t* real = px + one;
    // error of: generated, showing A (no generator), showing B, a plain mix of A and B
    double sum[4] = {}, bad[4] = {};
    for (size_t i = 0; i < (size_t)w * h; i++)
    {
        const uint8_t* g = px + i * 4;
        const uint8_t* m = real + i * 4;
        const uint8_t* a = px + one * 2 + i * 4;
        const uint8_t* b = px + one * 3 + i * 4;
        int e[4] = {}, mx[4] = {};
        for (int c = 0; c < 3; c++)
        {
            const int mix = (int)std::lround(a[c] * (1 - t) + b[c] * t);
            const int d[4] = { std::abs(g[c] - m[c]), std::abs(a[c] - m[c]), std::abs(b[c] - m[c]), std::abs(mix - m[c]) };
            for (int k = 0; k < 4; k++) { e[k] += d[k]; mx[k] = std::max(mx[k], d[k]); }
        }
        for (int k = 0; k < 4; k++) { sum[k] += e[k] / 3.0; if (mx[k] > 40) bad[k] += 1; }
    }
    const double n = (double)w * h;
    fprintf(stderr, "avaliacao %d (par %d): geo=%d  erro medio (0-255): gerado %.3f | so A %.3f | so B %.3f | mistura %.3f  "
                    "(>40: %.2f%% | %.2f%% | %.2f%% | %.2f%%)\n",
            sEvalDone + 1, sBuildNo, geo ? 1 : 0, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, bad[0] / n * 100, bad[1] / n * 100, bad[2] / n * 100,
            bad[3] / n * 100);
    if (geo)
    {
        sEvalGeo++;
        for (int k = 0; k < 4; k++) { sEvalSum[k] += sum[k] / n; sEvalBad[k] += bad[k] / n * 100; }
    }
    // PORTE_FGEVALBAD=1: only the pairs where the generated picture is worse than showing A or B
    static const bool onlyBad = getenv("PORTE_FGEVALBAD") != nullptr;
    const bool worse = sum[0] > std::min(sum[1], sum[2]) * 1.2 + 0.1 * n;
    const bool doDump = sEvalDone < dumpN && (!onlyBad || worse);
    if (doDump)
    {
        dump("gerado", px);
        dump("real", px + one);
        dump("A", px + one * 2);
        dump("B", px + one * 3);
    }
    if (doDump && geo)
    {
        S.runNow([&](vk::CommandBuffer cmd) {
            RecordOut(cmd, sSets[kEvalSet], i2, t, geo, nullptr, 0, true);
            cmd.copyImageToBuffer(sOut.image, vk::ImageLayout::eTransferSrcOptimal, sRead.buf, reg);
        });
        Invalidate(sRead, one);
        dump("origem", (const uint8_t*)sRead.map);
    }
    sEvalDone++;
    if (--sEvalLeft == 0 && sEvalGeo > 0)
        fprintf(stderr, "avaliacao media (%d pares com geometria): gerado %.3f | so A %.3f | so B %.3f | mistura %.3f  "
                        "(>40: %.2f%% | %.2f%% | %.2f%% | %.2f%%)\n",
                sEvalGeo, sEvalSum[0] / sEvalGeo, sEvalSum[1] / sEvalGeo, sEvalSum[2] / sEvalGeo, sEvalSum[3] / sEvalGeo, sEvalBad[0] / sEvalGeo,
                sEvalBad[1] / sEvalGeo, sEvalBad[2] / sEvalGeo, sEvalBad[3] / sEvalGeo);
    sPairDirty = true;   // back to the live pair
}

bool ReadOutputRect(int x, int y, int w, int h, std::vector<uint8_t>& bgra)
{
    if (!sOut.image || sOut.layout != vk::ImageLayout::eTransferSrcOptimal) return false;
    x = std::clamp(x, 0, (int)sW - 1); y = std::clamp(y, 0, (int)sH - 1);
    w = std::clamp(w, 1, (int)sW - x); h = std::clamp(h, 1, (int)sH - y);
    const size_t bytes = (size_t)w * h * 4;
    Ensure(sRead, bytes, vk::BufferUsageFlagBits::eTransferDst, true);
    S.runNow([&](vk::CommandBuffer cmd) {
        const vk::BufferImageCopy reg = { .bufferOffset = 0, .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                                          .imageOffset = { x, y, 0 }, .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
        cmd.copyImageToBuffer(sOut.image, vk::ImageLayout::eTransferSrcOptimal, sRead.buf, reg);
    });
    Invalidate(sRead, bytes);
    bgra.assign((const uint8_t*)sRead.map, (const uint8_t*)sRead.map + bytes);
    return true;
}

bool ReadOutput(std::vector<uint8_t>& bgra)
{
    if (!sOut.image || sOut.layout != vk::ImageLayout::eTransferSrcOptimal) return false;
    const size_t bytes = (size_t)sW * sH * 4;
    Ensure(sRead, bytes, vk::BufferUsageFlagBits::eTransferDst, true);
    S.runNow([&](vk::CommandBuffer cmd) {
        const vk::BufferImageCopy reg = { .bufferOffset = 0, .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                                          .imageExtent = { sW, sH, 1 } };
        cmd.copyImageToBuffer(sOut.image, vk::ImageLayout::eTransferSrcOptimal, sRead.buf, reg);
    });
    Invalidate(sRead, bytes);
    bgra.assign((const uint8_t*)sRead.map, (const uint8_t*)sRead.map + bytes);
    return true;
}

}   // namespace FGV
