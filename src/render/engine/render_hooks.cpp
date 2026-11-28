#include "scene_probe.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REX_EXTERN(__imp__sub_8288F858);  // Engine: pixel shader from an XDK container (r3), cached (header at object + 40).
REX_EXTERN(__imp__sub_828901C8);  // Engine: vertex shader from an XDK container (r3), cached (header at object + 872).
REX_EXTERN(__imp__sub_82AB7398);  // D3D DrawVertices(device, ...)
REX_EXTERN(__imp__sub_82AB77B0);  // D3D DrawIndexedVertices(device, ...)
REX_EXTERN(__imp__sub_82883D30);  // Engine: resolve the bound targets (renderer, ...)
REX_EXTERN(__imp__sub_82ABAB98);  // D3D Resolve(device, flags, ...)
REX_EXTERN(__imp__sub_82ABFD10);  // D3D Swap(device, ...)
REX_EXTERN(__imp__sub_82ABB148);  // D3D GpuLoadShaders(device, vs, ps, ...): the engine's shader binding (sub_828843C8)
REX_EXTERN(__imp__sub_82AB8850);  // D3D SetVertexShader(device, vs)
REX_EXTERN(__imp__sub_82AB8648);  // D3D SetPixelShader(device, ps)
REX_EXTERN(__imp__sub_82AB1178);  // D3D kick: submits the ring segment, starts a new one (returns its cursor)

namespace scene = fuse::engine::scene;

namespace
{
  // The title's call site of the engine resolve wrapper, for the D3D Resolve
  // it makes: per guest thread (its context), 0 outside the wrapper. Only the
  // render thread resolves; a host thread may run several guest fibers.
  struct ResolveSite
  {
    const PPCContext *context = nullptr;
    uint32_t site = 0;
  } resolve_site;
} // namespace

// Shader creation: the returned object identifies the container's shader.
REX_HOOK_RAW(sub_8288F858)
{
  const uint32_t container = ctx.r3.u32;
  __imp__sub_8288F858(ctx, base);
  if (scene::Enabled())
    scene::OnCreateShader(true, container, ctx.r3.u32);
}
REX_HOOK_RAW(sub_828901C8)
{
  const uint32_t container = ctx.r3.u32;
  __imp__sub_828901C8(ctx, base);
  if (scene::Enabled())
    scene::OnCreateShader(false, container, ctx.r3.u32);
}

// Draw / DrawIndexed(device, ...): the frame without the HUD before the first
// HUD draw; the camera marker after the first scene draw that has it.
REX_HOOK_RAW(sub_82AB7398)
{
  const uint32_t device = ctx.r3.u32;
  const bool active = scene::Active();
  if (active)
    scene::BeforeDraw(device);
  __imp__sub_82AB7398(ctx, base);
  if (active)
    scene::AfterDraw(device);
}
REX_HOOK_RAW(sub_82AB77B0)
{
  const uint32_t device = ctx.r3.u32;
  const bool active = scene::Active();
  if (active)
    scene::BeforeDraw(device);
  __imp__sub_82AB77B0(ctx, base);
  if (active)
    scene::AfterDraw(device);
}

// The engine's resolve: its caller identifies the pass.
REX_HOOK_RAW(sub_82883D30)
{
  const ResolveSite outer = resolve_site;
  resolve_site = {&ctx, uint32_t(ctx.lr)};
  __imp__sub_82883D30(ctx, base);
  resolve_site = outer;
}

// Resolve(device, flags): the scene end before the scene's resolve.
REX_HOOK_RAW(sub_82ABAB98)
{
  const uint32_t device = ctx.r3.u32;
  if (scene::Active())
  {
    const uint32_t site = resolve_site.context == &ctx && resolve_site.site ? resolve_site.site : uint32_t(ctx.lr);
    scene::BeforeResolve(device, site, ctx.r4.u32);
  }
  __imp__sub_82ABAB98(ctx, base);
  if (scene::Active())
    scene::AfterResolve(device);
}

namespace
{
  // Frame pacing as the game thread sees it, with frame_stats_interval (every
  // that many seconds, beside the renderer's lines): the time inside Swap
  // (D3D waiting for the GPU / the presentation interval) and outside it (game
  // and render submission), with the presentation interval D3D has (device +
  // 14000, set by the renderer's present sub_8288D570 from renderer + 1240) and
  // the renderer's frame time in vblanks (+16). The renderer is the global at
  // 0x833E3DF8 (sub_8288D0D0 fills it from the startup settings; the pause
  // state sub_826A7A88 / sub_826A7B90 rewrites both).
  struct SwapPacing
  {
    std::chrono::steady_clock::time_point window_start{}, last_end{};
    double inside = 0, outside = 0;
    uint32_t frames = 0, intervals[4]{};
  } swap_pacing;

  uint32_t LoadGuest32(uint8_t *base, uint32_t address)
  {
    uint32_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return __builtin_bswap32(value);
  }

  void NoteSwap(uint8_t *base, uint32_t device, std::chrono::steady_clock::time_point start,
                std::chrono::steady_clock::time_point end)
  {
    auto &p = swap_pacing;
    if (p.last_end != std::chrono::steady_clock::time_point{})
    {
      p.outside += std::chrono::duration<double, std::milli>(start - p.last_end).count();
      p.inside += std::chrono::duration<double, std::milli>(end - start).count();
      ++p.frames;
      ++p.intervals[std::min<uint32_t>(LoadGuest32(base, device + 14000), 3)];
    }
    else
      p.window_start = start;
    p.last_end = end;
    if (end - p.window_start < std::chrono::seconds(1) || !p.frames)
      return;
    // The GPU plugin's cvar (by name: it lives in the plugin).
    const int interval = std::atoi(rex::cvar::GetFlagByName("frame_stats_interval").c_str());
    if (interval <= 0 || end - p.window_start < std::chrono::seconds(interval))
    {
      if (interval <= 0)
      {
        p = {};
        p.window_start = p.last_end = end;
      }
      return;
    }
    constexpr uint32_t kRenderer = 0x833E3DF8;
    const double seconds = std::chrono::duration<double>(end - p.window_start).count();
    REXLOG_INFO("Fuse swap: {:.1f} fps; game thread {:.2f} ms a frame outside Swap, {:.2f} ms inside; D3D "
                "presentation interval 0/1/2/other {}/{}/{}/{} frames; renderer interval {} (last applied {}), "
                "frame time {:.2f} vblanks",
                p.frames / seconds, p.outside / p.frames, p.inside / p.frames, p.intervals[0], p.intervals[1],
                p.intervals[2], p.intervals[3], LoadGuest32(base, kRenderer + 1240),
                LoadGuest32(base, kRenderer + 1244), std::bit_cast<float>(LoadGuest32(base, kRenderer + 16)));
    p = {};
    p.window_start = p.last_end = end;
  }
} // namespace

// Swap(device, ...): the frame's end.
REX_HOOK_RAW(sub_82ABFD10)
{
  const uint32_t device = ctx.r3.u32;
  if (scene::Active())
    scene::BeforeSwap(device);
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_82ABFD10(ctx, base);
  NoteSwap(base, device, start, std::chrono::steady_clock::now());
  if (scene::Enabled())
    scene::OnSwap(device);
}

// Shader binding: GpuLoadShaders writes its packets directly and leaves the
// device's shader slots alone, so the producer keeps the bound pair itself.
REX_HOOK_RAW(sub_82ABB148)
{
  scene::OnBindShaders(ctx.r4.u32, ctx.r5.u32, true, true);
  __imp__sub_82ABB148(ctx, base);
}
REX_HOOK_RAW(sub_82AB8850)
{
  scene::OnBindShaders(ctx.r4.u32, 0, true, false);
  __imp__sub_82AB8850(ctx, base);
}
REX_HOOK_RAW(sub_82AB8648)
{
  scene::OnBindShaders(0, ctx.r4.u32, false, true);
  __imp__sub_82AB8648(ctx, base);
}

// Ring segment change: the producer reads the old segment to its end and
// goes on in the new one.
REX_HOOK_RAW(sub_82AB1178)
{
  const uint32_t device = ctx.r3.u32;
  const bool active = scene::Active();
  if (active)
    scene::BeforeKick(device);
  __imp__sub_82AB1178(ctx, base);
  if (active)
    scene::AfterKick(device);
}
