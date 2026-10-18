// Mid-asm hooks replacing the xenia-canary code patches for Fuse.
// Declared as [[midasm_hook]] entries in patches.toml. Every hook returns true
// when its patch is enabled (the generated code then skips the original
// instruction(s) and jumps to jump_address_on_true) and false otherwise.
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "game_cvars.h"

namespace
{
  // Guest memory is big-endian; hooks don't receive `base`, so go through the
  // kernel state like rex::ppc::ImportFunction does.
  float LoadGuestF32(uint32_t address)
  {
    auto *memory = rex::system::kernel_state()->memory();
    uint32_t be{};
    std::memcpy(&be, memory->TranslateVirtual(address), sizeof(be));
    return std::bit_cast<float>(__builtin_bswap32(be));
  }
} // namespace

// lfs f0,27448(r7): load the 60 FPS frame-time constant instead of the 30 FPS
// one at 27480(r7). Physics keeps its own fixed timestep.
bool FuseFps60Hook(PPCRegister &r7, PPCRegister &f0, PPCFPSCRRegister &fpscr)
{
  if (!REXCVAR_GET(fuse_fps60))
    return false;
  // The skipped lfs would have switched the FPU to non-flush mode.
  fpscr.disableFlushMode();
  f0.f64 = double(LoadGuestF32(r7.u32 + 27448));
  return true;
}
