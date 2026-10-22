#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "mouse_look.h"

namespace
{
  // The input component's local player slot.
  constexpr uint32_t kInputPlayerSlot = 56;
  // Keyboard and mouse are merged into guest user 0.
  constexpr uint32_t kMousePlayerSlot = 0;

  // Mover angle offsets.
  struct MoverAngles
  {
    uint32_t yaw;
    uint32_t pitch;
  };
  constexpr MoverAngles kFollow{564, 568};
  constexpr MoverAngles kLook{636, 640};
  constexpr MoverAngles kTurret{272, 276};

  // Follow modifier block in sub_824564B8's frame, and the follow mover's pitch limits.
  constexpr uint32_t kModifierYaw = 148;
  constexpr uint32_t kModifierPitch = 156;
  constexpr uint32_t kFollowPitchMin = 580;
  constexpr uint32_t kFollowPitchMax = 584;
  constexpr double kTwoPi = 6.28318530717958647692;

  // Mover whose stick read just succeeded for slot 0; taken by the turn hook of the same
  // update. Camera updates run on the game thread, one mover at a time.
  thread_local uint32_t g_owner_mover = 0;

  // Guest memory is big-endian; hooks don't receive `base`, so go through the
  // kernel state like rex::ppc::ImportFunction does.
  uint8_t *Translate(uint32_t address)
  {
    return rex::system::kernel_state()->memory()->TranslateVirtual(address);
  }

  uint32_t LoadGuestU32(uint32_t address)
  {
    uint32_t be{};
    std::memcpy(&be, Translate(address), sizeof(be));
    return __builtin_bswap32(be);
  }

  float LoadGuestF32(uint32_t address)
  {
    return std::bit_cast<float>(LoadGuestU32(address));
  }

  void StoreGuestF32(uint32_t address, float value)
  {
    const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(value));
    std::memcpy(Translate(address), &be, sizeof(be));
  }

  // Wraps the yaw to [-pi, pi] and clamps the pitch to [min, max], as step 4 would.
  struct Settle
  {
    uint32_t pitch_min_address;
    uint32_t pitch_max_address;
  };

  // Adds the mouse turn to the angles at yaw_address / pitch_address when `mover` is the
  // one the owner hook marked. settle: for angles step 4 will not see.
  void ApplyTurn(uint32_t mover, uint32_t yaw_address, uint32_t pitch_address, double dt,
                 const Settle *settle = nullptr)
  {
    if (mover == 0 || mover != g_owner_mover)
    {
      return;
    }
    g_owner_mover = 0;

    auto &look = fuse::mouse::MouseLook::Get();
    if (!look.Enabled())
    {
      return;
    }
    // Paused or a hitch: the motion stays pending for the next update.
    if (!(dt > 1e-4 && dt < 0.5))
    {
      return;
    }
    const auto turn = look.Consume();
    if (!turn)
    {
      return;
    }

    const float yaw_in = LoadGuestF32(yaw_address);
    const float pitch_in = LoadGuestF32(pitch_address);
    if (!std::isfinite(yaw_in) || !std::isfinite(pitch_in))
    {
      return;
    }
    // The movers' yaw grows to the left.
    double yaw = yaw_in - turn->yaw;
    double pitch = pitch_in + turn->pitch;
    if (settle)
    {
      yaw = std::remainder(yaw, kTwoPi);
      const double lo = LoadGuestF32(settle->pitch_min_address);
      const double hi = LoadGuestF32(settle->pitch_max_address);
      if (std::isfinite(lo) && std::isfinite(hi) && lo <= hi)
      {
        pitch = std::clamp(pitch, lo, hi);
      }
    }
    StoreGuestF32(yaw_address, static_cast<float>(yaw));
    StoreGuestF32(pitch_address, static_cast<float>(pitch));
  }

  void ApplyMoverTurn(const MoverAngles &angles, uint32_t mover, double dt)
  {
    ApplyTurn(mover, mover + angles.yaw, mover + angles.pitch, dt);
  }
} // namespace

// Step 1 succeeded: the hero has an input map and local input. r31 = mover, the second
// register = the hero's input component (r30, or r26 in the look camera).
void FuseMouseCameraOwner(PPCRegister &mover, PPCRegister &input)
{
  const uint32_t slot = LoadGuestU32(input.u32 + kInputPlayerSlot);
  if (slot != kMousePlayerSlot)
  {
    g_owner_mover = 0;
    return;
  }
  g_owner_mover = mover.u32;
  // The turn hook may be skipped this frame (camera transitions); the
  // player's camera is live all the same.
  auto &look = fuse::mouse::MouseLook::Get();
  if (look.Enabled())
  {
    look.KeepAlive();
  }
}

// Step 1 bailed out (no input map or no local input): the stick reads as zero.
void FuseMouseCameraNoOwner()
{
  g_owner_mover = 0;
}

// Step 4 of the follow camera, sub_82454F08(mover, dt).
void FuseMouseFollowTurn(PPCRegister &r3, PPCRegister &f1)
{
  ApplyMoverTurn(kFollow, r3.u32, f1.f64);
}

// The follow modifier moved the camera instead (sub_824564B8, 0x8245664C): r31 = mover,
// r1 = the frame holding the modifier's angles, f31 = dt. They are copied into the mover
// right after, and step 4 is skipped.
void FuseMouseFollowModifierTurn(PPCRegister &r31, PPCRegister &r1, PPCRegister &f31)
{
  const uint32_t mover = r31.u32;
  const Settle settle{mover + kFollowPitchMin, mover + kFollowPitchMax};
  ApplyTurn(mover, r1.u32 + kModifierYaw, r1.u32 + kModifierPitch, f31.f64, &settle);
}

// Step 4 of the look camera, sub_82458208(mover, dt).
void FuseMouseLookTurn(PPCRegister &r3, PPCRegister &f1)
{
  ApplyMoverTurn(kLook, r3.u32, f1.f64);
}

// Step 4 of the turret camera, sub_8245CC98(mover, dt).
void FuseMouseTurretTurn(PPCRegister &r3, PPCRegister &f1)
{
  ApplyMoverTurn(kTurret, r3.u32, f1.f64);
}
