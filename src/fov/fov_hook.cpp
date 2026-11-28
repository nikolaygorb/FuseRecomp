#include <algorithm>
#include <atomic>
#include <cmath>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "../game_cvars.h"
#include "fov.h"

REX_EXTERN(__imp__sub_828888B0);

namespace
{
  constexpr double kDegrees = 3.14159265358979323846 / 180.0;
  // sub_827BE7C0: a viewport's scene view; f3 = viewport fov (+232) * the scene's scale (+24).
  constexpr uint32_t kSceneViewBegin = 0x827BE7C0;
  constexpr uint32_t kSceneViewEnd = 0x827BE8A0;

  std::atomic<float> g_game_fov{0.f};
  std::atomic<float> g_applied_fov{0.f};
} // namespace

namespace fuse::fov
{
  double Apply(double game_fov)
  {
    const double scale = REXCVAR_GET(fuse_fov_scale);
    if (scale == 1.0 || !(game_fov > 0.0 && game_fov < 3.1))
      return game_fov;
    return std::min(2.0 * std::atan(std::tan(game_fov * 0.5) * scale), 3.1);
  }

  float LastGameFov() { return g_game_fov.load(std::memory_order_relaxed); }
  float LastAppliedFov() { return g_applied_fov.load(std::memory_order_relaxed); }
} // namespace fuse::fov

// Only the scene camera: shadow, cube map and light views keep their own frusta.
REX_HOOK_RAW(sub_828888B0)
{
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr >= kSceneViewBegin && lr < kSceneViewEnd)
  {
    const double game = ctx.f3.f64;
    const double applied = double(float(fuse::fov::Apply(game)));
    g_game_fov.store(float(game / kDegrees), std::memory_order_relaxed);
    g_applied_fov.store(float(applied / kDegrees), std::memory_order_relaxed);
    ctx.f3.f64 = applied;
  }
  __imp__sub_828888B0(ctx, base);
}
