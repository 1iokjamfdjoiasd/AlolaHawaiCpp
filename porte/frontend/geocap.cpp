#include "geocap.h"

#include "common/common_types.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/pica/output_vertex.h"
#include "video_core/pica/pica_core.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace GeoCap
{

static std::vector<Draw> sDraws;   // draws since the last new picture
static Draw sCur;

static Vtx Convert(const Pica::OutputVertex& o)
{
    Vtx v;
    for (int i = 0; i < 4; i++) v.pos[i] = o.pos[i].ToFloat32();
    v.tc0[0] = o.tc0[0].ToFloat32();
    v.tc0[1] = o.tc0[1].ToFloat32();
    for (int i = 0; i < 4; i++) v.color[i] = o.color[i].ToFloat32();
    return v;
}

// Forwards every call to Azahar's rasterizer; on the way, keeps the CPU-path triangles.
class Recorder : public VideoCore::RasterizerInterface
{
public:
    VideoCore::RasterizerInterface* inner = nullptr;

    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1, const Pica::OutputVertex& v2) override
    {
        sCur.v.push_back(Convert(v0));
        sCur.v.push_back(Convert(v1));
        sCur.v.push_back(Convert(v2));
        inner->AddTriangle(v0, v1, v2);
    }
    void DrawTriangles() override
    {
        if (!sCur.v.empty())
        {
            const auto& regs = Core::System::GetInstance().GPU().PicaCore().regs.internal;
            sCur.colorAddr = regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress();
            sCur.depthAddr = regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress();
            sCur.vpW = Pica::f24::FromRaw(regs.rasterizer.viewport_size_x).ToFloat32() * 2;
            sCur.vpH = Pica::f24::FromRaw(regs.rasterizer.viewport_size_y).ToFloat32() * 2;
            // which texture and mesh it is, and how it is tested / blended
            const auto& tx = regs.texturing;
            sCur.tex0 = tx.main_config.texture0_enable ? tx.texture0.GetPhysicalAddress() : 0;
            const auto& pl = regs.pipeline;
            sCur.mesh = pl.vertex_attributes.GetPhysicalBaseAddress() + (u32)pl.vertex_attributes.attribute_loaders[0].data_offset
                        + (u32)pl.index_array.offset * 31u;
            const auto& om = regs.framebuffer.output_merger;
            const auto& fb = regs.framebuffer.framebuffer;
            const u32 src = (u32)om.alpha_blending.factor_source_rgb.Value(), dst = (u32)om.alpha_blending.factor_dest_rgb.Value();
            u32 st = 0;
            if (om.alphablend_enable && !(src == 1 && dst == 0)) st |= kBlend;
            if (om.depth_test_enable) st |= kDepthTest;
            st |= ((u32)om.depth_test_func.Value() & 7u) << kDepthFuncShift;
            if (om.depth_write_enable && fb.allow_depth_stencil_write != 0) st |= kDepthWrite;
            st |= ((u32)regs.rasterizer.cull_mode.Value() & 3u) << kCullShift;
            if (om.alpha_test.enable) st |= kAlphaTest;
            if (regs.rasterizer.depthmap_enable == Pica::RasterizerRegs::DepthBuffering::WBuffering) st |= kWBuffer;
            if (fb.allow_color_write != 0 && (om.red_enable || om.green_enable || om.blue_enable)) st |= kColorWrite;
            if (om.stencil_test.enable) st |= kStencilTest;
            st |= (src & 15u) << kSrcShift;
            st |= (dst & 15u) << kDstShift;
            sCur.state = st;
            sCur.depthScale = Pica::f24::FromRaw(regs.rasterizer.viewport_depth_range).ToFloat32();
            sCur.depthOffset = Pica::f24::FromRaw(regs.rasterizer.viewport_depth_near_plane).ToFloat32();
            sDraws.push_back(std::move(sCur));
            sCur = Draw{};
        }
        inner->DrawTriangles();
    }
    void FlushAll() override { inner->FlushAll(); }
    void FlushRegion(PAddr a, u32 s) override { inner->FlushRegion(a, s); }
    void InvalidateRegion(PAddr a, u32 s) override { inner->InvalidateRegion(a, s); }
    void FlushAndInvalidateRegion(PAddr a, u32 s) override { inner->FlushAndInvalidateRegion(a, s); }
    void ClearAll(bool flush) override { inner->ClearAll(flush); }
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& c) override { return inner->AccelerateDisplayTransfer(c); }
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& c) override { return inner->AccelerateTextureCopy(c); }
    bool AccelerateFill(const Pica::MemoryFillConfig& c) override { return inner->AccelerateFill(c); }
    bool AccelerateDrawBatch(bool is_indexed) override { return inner->AccelerateDrawBatch(is_indexed); }
    void LoadDefaultDiskResources(const std::atomic_bool& stop, const VideoCore::DiskResourceLoadCallback& cb) override
    {
        inner->LoadDefaultDiskResources(stop, cb);
    }
    void SwitchDiskResources(u64 title_id) override { inner->SwitchDiskResources(title_id); }
};

static Recorder sRecorder;

void Install()
{
    auto& gpu = Core::System::GetInstance().GPU();
    VideoCore::RasterizerInterface* real = gpu.Renderer().Rasterizer();
    if (!real) return;
    if (real != sRecorder.inner)
    {
        // a new renderer (state load, restart): what was recorded belonged to the old one
        sRecorder.inner = real;
        sDraws.clear();
        sCur = Draw{};
    }
    gpu.PicaCore().BindRasterizer(&sRecorder);
}

std::shared_ptr<const Image> OnFrameEnd(bool newImage)
{
    if (!newImage) return nullptr;   // draws keep adding up until the game shows a new picture
    auto img = std::make_shared<Image>();
    img->draws = std::move(sDraws);
    sDraws.clear();
    return img;
}

}
