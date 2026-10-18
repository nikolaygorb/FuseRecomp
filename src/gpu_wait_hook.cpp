// sub_82AB1960 is the XDK poll behind BlockUntilRingSpace: it returns nonzero to keep
// waiting for the GPU to free command-ring space and zero once the wait is over. The
// original poll is a few nops, so the render thread spins flat out while the emulated
// GPU is behind. Only the pause between polls changes; the result is untouched.
//
// Found by the poll's shape (cctpl; 8x db16cyc loop; cctpm; lbz r11,11069(r29)); this
// XDK build keeps the device fields 128 bytes later than Wet's/RDR's (10941 there).
#include <chrono>
#include <thread>

#include <rex/hook.h>

#include "game_cvars.h"

REX_EXTERN(__imp__sub_82AB1960);

namespace
{
  enum GpuWaitMode : int32_t
  {
    kBusySpin = 0,
    kYield = 1,
    kSleep = 2,
  };

  constexpr std::chrono::microseconds kSleepPause{200};
} // namespace

REX_HOOK_RAW(sub_82AB1960)
{
  __imp__sub_82AB1960(ctx, base);
  if (ctx.r3.u32 == 0)
    return;

  switch (REXCVAR_GET(fuse_gpu_wait_mode))
  {
  case kYield:
    std::this_thread::yield();
    break;
  case kSleep:
    std::this_thread::sleep_for(kSleepPause);
    break;
  default:
    break;
  }
}
