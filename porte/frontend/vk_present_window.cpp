// Replaces Azahar's src/video_core/renderer_vulkan/vk_present_window.cpp in this program
// (this object is linked before the core's library, so the core's own one is never pulled
// in). Same class, same interface, same frames - but instead of copying each frame to the
// swapchain, its own thread:
//   1. copies every frame with a new picture into the frame generator's history (with the
//      triangles and screen positions the frontend recorded for it, see porte_bridge.h),
//   2. presents at the chosen rate (200 FPS by default): the in-between picture for that
//      moment, the menu on top, then the swapchain (mailbox = no tearing, no cap).
// Copyright of the original: Citra Emulator Project / Azahar Emulator Project, GPLv2+.

#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <SDL.h>

#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

#include <vk_mem_alloc.h>

#include "framegen_vk.h"
#include "porte_bridge.h"

namespace Porte
{
Hooks gHooks;
std::atomic<bool> gGenOn{ false }, gVsync{ true }, gHold{ false }, gMenuOpen{ false };
std::atomic<int> gRate{ 200 }, gFrameSkip{ 0 }, gIngested{ 0 };
const uint8_t* gAtlas = nullptr;
const uint8_t* gPics = nullptr;
int gAtlasW = 0, gAtlasH = 0;
}

namespace Vulkan
{
namespace
{
std::mutex sMetaMutex;
std::deque<Porte::FrameMeta> sMetas;   // one per frame given to Present, in order
PresentWindow* sMain = nullptr;        // the window that generates frames (the first one)
double Now() { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
constexpr int kInFlight = 3;
}   // namespace

PresentWindow::PresentWindow(Frontend::EmuWindow& emu_window_, const Instance& instance_, Scheduler& scheduler_, bool low_refresh_rate_)
    : emu_window{ emu_window_ }, instance{ instance_ }, scheduler{ scheduler_ }, low_refresh_rate{ low_refresh_rate_ },
      surface{ CreateSurface(instance.GetInstance(), emu_window) }, next_surface{ surface },
      swapchain{ instance, emu_window.GetFramebufferLayout().width, emu_window.GetFramebufferLayout().height, surface, true },
      graphics_queue{ instance.GetGraphicsQueue() }, present_renderpass{ CreateRenderpass() },
      vsync_enabled{ Settings::values.use_vsync.GetValue() }, blit_supported{ true }, use_present_thread{ true },
      last_render_surface{ emu_window.GetWindowInfo().render_surface }
{
    const vk::FormatProperties props = instance.GetPhysicalDevice().getFormatProperties(swapchain.GetSurfaceFormat().format);
    blit_supported = static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
    // (16: the interpolation above 60 FPS hands over several in-between pictures at once, held until their time)
    const u32 num_images = std::max(16u, swapchain.GetImageCount());
    const vk::Device device = instance.GetDevice();
    command_pool = device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    });
    const std::vector command_buffers = device.allocateCommandBuffers({
        .commandPool = command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = num_images,
    });
    swap_chain.resize(num_images);
    for (u32 i = 0; i < num_images; i++)
    {
        Frame& frame = swap_chain[i];
        frame.cmdbuf = command_buffers[i];
        frame.render_ready = device.createSemaphore({});
        frame.present_done = device.createFence({ .flags = vk::FenceCreateFlagBits::eSignaled });
        free_queue.push(&frame);
    }
    // the second window (two monitors) never generates frames: it only shows the other screen
    if (!sMain && !emu_window.isSecondary()) sMain = this;
    present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
}

PresentWindow::~PresentWindow()
{
    present_thread.request_stop();
    frame_cv.notify_all();
    if (present_thread.joinable()) present_thread.join();
    scheduler.Finish();
    {
        std::scoped_lock lock{ scheduler.submit_mutex };
        graphics_queue.waitIdle();
    }
    const vk::Device device = instance.GetDevice();
    if (next_surface && next_surface != surface)
    {
        instance.GetInstance().destroySurfaceKHR(next_surface);
        next_surface = vk::SurfaceKHR{};
    }
    device.destroyCommandPool(command_pool);
    device.destroyRenderPass(present_renderpass);
    for (auto& frame : swap_chain)
    {
        device.destroyImageView(frame.image_view);
        device.destroyFramebuffer(frame.framebuffer);
        device.destroySemaphore(frame.render_ready);
        device.destroyFence(frame.present_done);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    }
    if (sMain == this) sMain = nullptr;
    std::scoped_lock lk{ sMetaMutex };
    sMetas.clear();
}

void PresentWindow::RecreateFrame(Frame* frame, u32 width, u32 height)
{
    vk::Device device = instance.GetDevice();
    if (frame->framebuffer) device.destroyFramebuffer(frame->framebuffer);
    if (frame->image_view) device.destroyImageView(frame->image_view);
    if (frame->image) vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };
    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
    if (vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info, &unsafe_image, &frame->allocation, nullptr) != VK_SUCCESS)
    {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating frame image");
        UNREACHABLE();
    }
    frame->image = vk::Image{ unsafe_image };
    frame->image_view = device.createImageView({
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    });
    frame->framebuffer = device.createFramebuffer({
        .renderPass = present_renderpass,
        .attachmentCount = 1,
        .pAttachments = &frame->image_view,
        .width = width,
        .height = height,
        .layers = 1,
    });
    frame->width = width;
    frame->height = height;
}

Frame* PresentWindow::GetRenderFrame()
{
    std::unique_lock lock{ free_mutex };
    free_cv.wait(lock, [this] { return !free_queue.empty(); });
    Frame* frame = free_queue.front();
    free_queue.pop();
    vk::Device device = instance.GetDevice();
    for (;;)
    {
        const vk::Result r = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        if (r == vk::Result::eSuccess) break;
    }
    device.resetFences(frame->present_done);
    return frame;
}

std::size_t PresentWindow::FreeFrameCount()
{
    std::scoped_lock lock{ free_mutex };
    return free_queue.size();
}

// (interpolation above 60) how many frames the main window has free: GetRenderFrame waits when there is none,
// and that wait stopped the emulation (game at ~96% at 3x resolution) - in-between pictures are skipped instead
int PorteFreeFrames()
{
    return sMain ? (int)sMain->FreeFrameCount() : 99;
}

void PresentWindow::Present(Frame* frame)
{
    // emulation thread: what the frontend knows about this frame (new picture? triangles?)
    // (only the main window's frames have one: the second window's presenter reads none)
    if (this == sMain)
    {
        Porte::FrameMeta meta;
        if (Porte::gHooks.onSwap) meta = Porte::gHooks.onSwap();
        std::scoped_lock lk{ sMetaMutex };
        sMetas.push_back(std::move(meta));
    }
    // the frame goes to the presenter after the core's submission that signals render_ready
    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{ queue_mutex };
        present_queue.push(frame);
        frame_cv.notify_all();
    });
    scheduler.DispatchWork();
}

void PresentWindow::WaitPresent()
{
    {
        std::unique_lock queue_lock{ queue_mutex };
        frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });
    }
    std::scoped_lock swapchain_lock{ swapchain_mutex };
}

void PresentWindow::NotifySurfaceChanged() {}

// not used: frames reach the swapchain through the frame generator (PresentThread)
void PresentWindow::CopyToSwapchain(Frame*) {}

void PresentWindow::PresentThread(std::stop_token token)
{
    Common::SetCurrentThreadName("PorteVkPresent");
    const vk::Device device = instance.GetDevice();
    const vk::CommandPool pool = device.createCommandPool({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    });
    const auto cmds = device.allocateCommandBuffers({ .commandPool = pool, .level = vk::CommandBufferLevel::ePrimary,
                                                      .commandBufferCount = kInFlight + 1 });
    vk::Fence fences[kInFlight];
    for (auto& f : fences) f = device.createFence({ .flags = vk::FenceCreateFlagBits::eSignaled });
    const vk::Fence onceFence = device.createFence({});
    const vk::CommandBuffer once = cmds[kInFlight];
    const auto waitFence = [&](vk::Fence f) {
        while (device.waitForFences(f, true, std::numeric_limits<u64>::max()) != vk::Result::eSuccess) {}
    };
    const auto runNow = [&](const std::function<void(vk::CommandBuffer)>& rec) {
        once.reset();
        once.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
        rec(once);
        once.end();
        {
            std::scoped_lock lk{ scheduler.submit_mutex };
            graphics_queue.submit(vk::SubmitInfo{ .commandBufferCount = 1, .pCommandBuffers = &once }, onceFence);
        }
        waitFence(onceFence);
        device.resetFences(onceFence);
        std::scoped_lock lk{ scheduler.submit_mutex };
        graphics_queue.waitIdle();
    };

    // Second window (two monitors, the other 3DS screen): no frame generation. The newest
    // real frame is copied to its swapchain, at most 60 times a second, never waiting on the
    // monitor (mailbox), so it costs the main window almost nothing.
    const auto secondWindowLoop = [&] {
        double credit = 1.0, lastT = Now();
        while (!token.stop_requested())
        {
            std::vector<Frame*> got;
            {
                std::unique_lock lock{ queue_mutex };
                if (present_queue.empty())
                    frame_cv.wait_for(lock, token, std::chrono::milliseconds(100), [this] { return !present_queue.empty(); });
                while (!present_queue.empty())
                {
                    got.push_back(present_queue.front());
                    present_queue.pop();
                }
            }
            frame_cv.notify_all();
            if (got.empty()) continue;
            std::scoped_lock sl{ swapchain_mutex };
            const double now = Now();
            credit = std::min(1.5, credit + (now - lastT) * 60.0);   // 60 per second, small jitter allowed
            lastT = now;
            for (size_t i = 0; i < got.size(); i++)
            {
                Frame* f = got[i];
                bool shown = false;
                f->cmdbuf.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
                if (i + 1 == got.size() && credit >= 1.0 && !token.stop_requested())
                {
                    const auto remake = [&] {
                        std::scoped_lock lk{ scheduler.submit_mutex };
                        graphics_queue.waitIdle();
                        swapchain.Create(f->width, f->height, surface, true);
                    };
                    if (swapchain.GetWidth() != f->width || swapchain.GetHeight() != f->height) remake();
                    int tries = 0;
                    bool ok;
                    while (!(ok = swapchain.AcquireNextImage()) && tries++ < 3) remake();
                    if (ok)
                    {
                        const vk::Image dst = swapchain.Image();
                        const vk::Extent2D ext = swapchain.GetExtent();
                        const vk::ImageSubresourceRange range = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
                        const vk::ImageMemoryBarrier pre = {
                            .srcAccessMask = {}, .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                            .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = dst, .subresourceRange = range,
                        };
                        f->cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                                  vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, pre);
                        const vk::ImageSubresourceLayers sub = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                        if (ext.width == f->width && ext.height == f->height)
                        {
                            const vk::ImageCopy c = { .srcSubresource = sub, .dstSubresource = sub, .extent = { f->width, f->height, 1 } };
                            f->cmdbuf.copyImage(f->image, vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eTransferDstOptimal, c);
                        }
                        else if (blit_supported)
                        {
                            const vk::ImageBlit b = {
                                .srcSubresource = sub,
                                .srcOffsets = std::array{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ (s32)f->width, (s32)f->height, 1 } },
                                .dstSubresource = sub,
                                .dstOffsets = std::array{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ (s32)ext.width, (s32)ext.height, 1 } },
                            };
                            f->cmdbuf.blitImage(f->image, vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eTransferDstOptimal, b,
                                                vk::Filter::eLinear);
                        }
                        else
                        {
                            const vk::ImageCopy c = { .srcSubresource = sub, .dstSubresource = sub,
                                                      .extent = { std::min(f->width, ext.width), std::min(f->height, ext.height), 1 } };
                            f->cmdbuf.copyImage(f->image, vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eTransferDstOptimal, c);
                        }
                        const vk::ImageMemoryBarrier post = {
                            .srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
                            .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::ePresentSrcKHR,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = dst, .subresourceRange = range,
                        };
                        f->cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, post);
                        shown = true;
                    }
                }
                // testing aid: PORTE_SHOT2AT=SEC saves the second window's picture once (shot2.raw
                // next to the data, int w, int h, BGRA rows)
                static const double shot2At = getenv("PORTE_SHOT2AT") ? atof(getenv("PORTE_SHOT2AT")) : 0;
                static const double t0 = Now();
                static bool shot2Done = false;
                vk::Buffer shotBuf{};
                vk::DeviceMemory shotMem{};
                if (shot2At > 0 && !shot2Done && Now() - t0 >= shot2At)
                {
                    shot2Done = true;
                    const vk::DeviceSize size = (vk::DeviceSize)f->width * f->height * 4;
                    shotBuf = device.createBuffer({ .size = size, .usage = vk::BufferUsageFlagBits::eTransferDst });
                    const auto req = device.getBufferMemoryRequirements(shotBuf);
                    const auto mp = instance.GetPhysicalDevice().getMemoryProperties();
                    u32 type = 0;
                    for (u32 k = 0; k < mp.memoryTypeCount; k++)
                        if ((req.memoryTypeBits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible) &&
                            (mp.memoryTypes[k].propertyFlags & vk::MemoryPropertyFlagBits::eHostCoherent)) { type = k; break; }
                    shotMem = device.allocateMemory({ .allocationSize = req.size, .memoryTypeIndex = type });
                    device.bindBufferMemory(shotBuf, shotMem, 0);
                    const vk::BufferImageCopy c = { .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
                                                    .imageExtent = { f->width, f->height, 1 } };
                    f->cmdbuf.copyImageToBuffer(f->image, vk::ImageLayout::eTransferSrcOptimal, shotBuf, c);
                }
                f->cmdbuf.end();
                const auto saveShot = [&] {
                    if (!shotBuf) return;
                    while (device.waitForFences(f->present_done, true, std::numeric_limits<u64>::max()) != vk::Result::eSuccess) {}
                    const void* data = device.mapMemory(shotMem, 0, VK_WHOLE_SIZE);
                    const std::string dir = getenv("PORTE_DATA") ? getenv("PORTE_DATA") : ".";
                    if (FILE* fp = fopen((dir + "\\shot2.raw").c_str(), "wb"))
                    {
                        const int hdr[2] = { (int)f->width, (int)f->height };
                        fwrite(hdr, sizeof(hdr), 1, fp);
                        fwrite(data, 1, (size_t)f->width * f->height * 4, fp);
                        fclose(fp);
                    }
                    device.unmapMemory(shotMem);
                    device.destroyBuffer(shotBuf);
                    device.freeMemory(shotMem);
                };
                // the core's render_ready is always waited on (a binary semaphore must be consumed)
                const vk::Semaphore waits[2] = { f->render_ready, shown ? swapchain.GetImageAcquiredSemaphore() : vk::Semaphore{} };
                static constexpr vk::PipelineStageFlags stages[2] = { vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer };
                const vk::Semaphore ready = shown ? swapchain.GetPresentReadySemaphore() : vk::Semaphore{};
                const vk::SubmitInfo si = {
                    .waitSemaphoreCount = shown ? 2u : 1u,
                    .pWaitSemaphores = waits,
                    .pWaitDstStageMask = stages,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &f->cmdbuf,
                    .signalSemaphoreCount = shown ? 1u : 0u,
                    .pSignalSemaphores = &ready,
                };
                {
                    std::scoped_lock lk{ scheduler.submit_mutex };
                    graphics_queue.submit(si, f->present_done);
                    if (shown) swapchain.Present();
                }
                saveShot();
                if (shown) credit -= 1.0;
                // stats (log): pictures the second window showed
                static int count = 0;
                static double since = Now();
                count += shown;
                if (Now() - since >= 5.0)
                {
                    fprintf(stderr, "segunda janela: %.1f imagens/s (%ux%u)\n", count / (Now() - since), f->width, f->height);
                    count = 0;
                    since = Now();
                }
                std::scoped_lock fl{ free_mutex };
                free_queue.push(f);
                free_cv.notify_one();
            }
        }
    };
    const bool generator = this == sMain;
    if (!generator)
    {
        secondWindowLoop();
        {
            std::scoped_lock lk{ scheduler.submit_mutex };
            graphics_queue.waitIdle();
        }
        device.destroyFence(onceFence);
        for (auto& f : fences) device.destroyFence(f);
        device.destroyCommandPool(pool);
        return;
    }
    bool fgOk = false;
    if (generator)
    {
        FGV::Setup s;
        s.dev = device;
        s.phys = instance.GetPhysicalDevice();
        s.alloc = instance.GetAllocator();
        s.format = swapchain.GetSurfaceFormat().format;
        s.runNow = runNow;
        s.atlas = Porte::gAtlas;
        s.atlasW = Porte::gAtlasW;
        s.atlasH = Porte::gAtlasH;
        s.pics = Porte::gPics;
        fgOk = FGV::Init(s);
        if (!fgOk) fprintf(stderr, "ERRO: o gerador de quadros Vulkan nao iniciou\n");
        fprintf(stderr, "vulkan: apresentacao propria, formato %d, %u imagens na swapchain\n", (int)s.format, swapchain.GetImageCount());
    }

    const auto recreate = [&](u32 w, u32 h) {
        Settings::values.use_vsync.SetValue(Porte::gVsync.load());
        vsync_enabled = Porte::gVsync;
        std::scoped_lock lk{ scheduler.submit_mutex };
        graphics_queue.waitIdle();
        // with "VSync" on the swapchain is created in mailbox mode (no tearing, no 60 FPS cap)
        swapchain.Create(w, h, surface, true);
    };

    std::vector<Porte::UiVert> ui;
    std::vector<u8> shot;
    double nextDue = 0, last = 0;
    int slot = 0;
    bool fresh = false;
    std::deque<std::pair<Frame*, Porte::FrameMeta>> held;   // in-between pictures waiting for their time
    while (!token.stop_requested())
    {
        // 1. frames from the core
        std::vector<Frame*> got;
        {
            std::unique_lock lock{ queue_mutex };
            if (present_queue.empty())
            {
                const bool gen = Porte::gGenOn && !Porte::gHold;
                double next = gen ? nextDue : last + (Porte::gMenuOpen ? 1.0 / 60 : 0.25);
                if (!held.empty()) next = std::min(next, held.front().second.showAt);
                const double wait = next - Now() - 0.0012;
                if (wait > 0)
                    frame_cv.wait_for(lock, token, std::chrono::duration<double>(wait), [this] { return !present_queue.empty(); });
            }
            while (!present_queue.empty())
            {
                got.push_back(present_queue.front());
                present_queue.pop();
            }
        }
        frame_cv.notify_all();
        if (token.stop_requested()) break;
        // in-between pictures of the interpolation come with a time to be shown: held until then
        std::vector<std::pair<Frame*, Porte::FrameMeta>> toShow;
        for (Frame* f : got)
        {
            Porte::FrameMeta m;
            {
                std::scoped_lock lk{ sMetaMutex };
                if (!sMetas.empty()) { m = std::move(sMetas.front()); sMetas.pop_front(); }
            }
            if (m.showAt > 0 || !held.empty()) held.push_back({ f, std::move(m) });   // (keeps the order)
            else toShow.push_back({ f, std::move(m) });
        }
        while (!held.empty() && held.front().second.showAt <= Now() + 0.0004)
        {
            toShow.push_back(std::move(held.front()));
            held.pop_front();
        }
        if (!toShow.empty())
        {
            std::scoped_lock sl{ swapchain_mutex };
            if (toShow.size() > 1) Porte::gFrameSkip += (int)toShow.size() - 1;
            for (auto& [f, m] : toShow)
            {
                if (fgOk && (f->width != FGV::Width() || f->height != FGV::Height())) FGV::Resize(f->width, f->height);
                f->cmdbuf.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
                if (fgOk) FGV::Ingest(f->cmdbuf, f->image, m, Now());
                f->cmdbuf.end();
                static constexpr vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
                const vk::SubmitInfo si = {
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &f->render_ready,
                    .pWaitDstStageMask = &waitStage,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &f->cmdbuf,
                };
                {
                    std::scoped_lock lk{ scheduler.submit_mutex };
                    graphics_queue.submit(si, f->present_done);
                }
                if (m.changed) { fresh = true; Porte::gIngested++; }
                std::scoped_lock fl{ free_mutex };
                free_queue.push(f);
                free_cv.notify_one();
            }
        }
        if (!fgOk) continue;

        // 2. a frame on the screen when it is due
        const double now = Now();
        const bool hold = Porte::gHold;
        const bool gen = Porte::gGenOn && !hold;
        const double interval = 1.0 / std::max(1, Porte::gRate.load());
        const bool due = gen ? now >= nextDue : (fresh || now - last >= (Porte::gMenuOpen ? 1.0 / 60 : 0.25));
        if (!due || !FGV::Ready())
        {
            // close to the next frame: keep checking without sleeping (sleeps run late)
            if (gen && nextDue - Now() < 0.0012) std::this_thread::yield();
            continue;
        }
        fresh = false;
        slot = (slot + 1) % kInFlight;
        if (FGV::PairPending()) waitFence(fences[0]), waitFence(fences[1]), waitFence(fences[2]);
        else waitFence(fences[slot]);

        const u32 W = FGV::Width(), H = FGV::Height();
        if (vsync_enabled != Porte::gVsync.load() || swapchain.GetWidth() != W || swapchain.GetHeight() != H) recreate(W, H);
        int tries = 0;
        while (!swapchain.AcquireNextImage() && tries++ < 5) recreate(W, H);
        device.resetFences(fences[slot]);

        ui.clear();
        if (Porte::gHooks.buildUi) Porte::gHooks.buildUi((int)W, (int)H, ui);
        const vk::CommandBuffer cmd = cmds[slot];
        cmd.reset();
        cmd.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
        bool generated = false;
        FGV::Render(cmd, now, slot, gen, hold, ui, generated);

        // the output image -> the swapchain
        const vk::Image dst = swapchain.Image();
        const vk::Extent2D ext = swapchain.GetExtent();
        const vk::ImageMemoryBarrier pre = {
            .srcAccessMask = {},
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eColorAttachmentOutput,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, pre);
        const vk::ImageSubresourceLayers sub = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        if (ext.width == W && ext.height == H)
        {
            const vk::ImageCopy c = { .srcSubresource = sub, .dstSubresource = sub, .extent = { W, H, 1 } };
            cmd.copyImage(FGV::Output(), vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eTransferDstOptimal, c);
        }
        else if (blit_supported)
        {
            const vk::ImageBlit b = {
                .srcSubresource = sub,
                .srcOffsets = std::array{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ (s32)W, (s32)H, 1 } },
                .dstSubresource = sub,
                .dstOffsets = std::array{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ (s32)ext.width, (s32)ext.height, 1 } },
            };
            cmd.blitImage(FGV::Output(), vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eTransferDstOptimal, b, vk::Filter::eLinear);
        }
        else
        {
            const vk::ImageCopy c = { .srcSubresource = sub, .dstSubresource = sub,
                                      .extent = { std::min(W, ext.width), std::min(H, ext.height), 1 } };
            cmd.copyImage(FGV::Output(), vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eTransferDstOptimal, c);
        }
        const vk::ImageMemoryBarrier post = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, post);
        cmd.end();

        static constexpr vk::PipelineStageFlags acquireStage = vk::PipelineStageFlagBits::eTransfer;
        const vk::Semaphore acquired = swapchain.GetImageAcquiredSemaphore();
        const vk::Semaphore ready = swapchain.GetPresentReadySemaphore();
        const vk::SubmitInfo si = {
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &acquireStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &ready,
        };
        {
            std::scoped_lock lk{ scheduler.submit_mutex };
            try
            {
                graphics_queue.submit(si, fences[slot]);
            }
            catch (vk::DeviceLostError& err)
            {
                LOG_CRITICAL(Render_Vulkan, "Device lost during present submit: {}", err.what());
                UNREACHABLE();
            }
            swapchain.Present();
        }
        if (Porte::gHooks.onPresented) Porte::gHooks.onPresented(generated);
        if (Porte::gLatRec)   // test (PORTE_LATENCIA): when each picture went to the screen, and how much it changed
        {
            const double tp = Now();
            static std::vector<u8> prev, cur;
            static int rx = -1, ry, rw, rh;
            if (rx < 0)
            {
                rx = 886; ry = 170; rw = 160; rh = 200;
                if (const char* r = getenv("PORTE_LAT_RET")) sscanf(r, "%d,%d,%d,%d", &rx, &ry, &rw, &rh);
            }
            if (FGV::ReadOutputRect(rx, ry, rw, rh, cur))
            {
                double dif = 0;
                if (prev.size() == cur.size())
                {
                    for (size_t i = 0; i < cur.size(); i++) dif += std::abs((int)cur[i] - (int)prev[i]);
                    dif /= (double)cur.size();
                }
                fprintf(stderr, "lat: imagem t=%.6f dif=%.3f\n", tp, dif);
                if (const char* dir = getenv("PORTE_LAT_SALVAR"))   // (test) the piece itself, one file per picture
                {
                    static int k = 0;
                    const std::string path = std::string(dir) + "/lat_" + std::to_string(++k) + ".raw";
                    if (FILE* f = fopen(path.c_str(), "wb"))
                    {
                        const int hd[2] = { rw, rh };
                        fwrite(hd, sizeof(hd), 1, f);
                        fwrite(cur.data(), 1, cur.size(), f);
                        fclose(f);
                    }
                }
                prev.swap(cur);
            }
        }
        if (Porte::gHooks.shotWanted)
        {
            const std::string file = Porte::gHooks.shotWanted();
            if (!file.empty() && FGV::ReadOutput(shot))
            {
                if (FILE* fp = _wfopen(std::filesystem::path(std::u8string((const char8_t*)file.c_str())).c_str(), L"wb"))
                {
                    const int hdr[2] = { (int)W, (int)H };
                    fwrite(hdr, sizeof(hdr), 1, fp);
                    fwrite(shot.data(), 1, shot.size(), fp);
                    fclose(fp);
                }
            }
        }
        if (FGV::EvalPending()) FGV::RunEval();
        last = Now();
        // keep a steady beat (no drift), but never queue up a burst after a stall
        nextDue = std::max(nextDue + interval, last + interval * 0.5);
    }

    {
        std::scoped_lock lk{ scheduler.submit_mutex };
        graphics_queue.waitIdle();
    }
    if (fgOk) FGV::Shutdown();
    for (auto& f : fences) device.destroyFence(f);
    device.destroyFence(onceFence);
    device.destroyCommandPool(pool);
}

vk::RenderPass PresentWindow::CreateRenderpass()
{
    const vk::AttachmentReference color_ref = { .attachment = 0, .layout = vk::ImageLayout::eGeneral };
    const vk::SubpassDescription subpass = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &color_ref,
    };
    const vk::AttachmentDescription color_attachment = {
        .format = swapchain.GetSurfaceFormat().format,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eTransferSrcOptimal,
    };
    return instance.GetDevice().createRenderPass({
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    });
}

}   // namespace Vulkan
