// game_constants.h - Game-specific constants
#pragma once

#include <cstdint>

namespace GameConstants
{
  constexpr uint32_t kCodeBase = 0x82250000;
  constexpr uint32_t kCodeEnd = 0x8313F2F4;
}

namespace GameConstants::PatchConstants
{
  struct Patch
  {
    std::uintptr_t address;
    std::uint32_t value;
    std::uint32_t width;
  };

  constexpr Patch SkipIntro() { return Patch{0x82089A43, 0x00, 1}; }

  constexpr Patch TextureStreamingFix() { return Patch{0x8346AAB1, 0x01, 1}; }
}
