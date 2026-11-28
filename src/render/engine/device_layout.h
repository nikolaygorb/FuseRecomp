#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fuse::engine
{
  inline constexpr uint32_t kDeviceBytes = 14096;
  // Command ring: the D3D code checks cursor <= safe end, then writes with stwu
  // (the cursor is the last written word); reserving is its own business.
  inline constexpr uint32_t kRingCursor = 48, kRingSafeEnd = 56;
  inline constexpr uint32_t kConstantBegin = 1920;  // VS float constants c0..c255 (big-endian).
  inline constexpr uint32_t kColorInfo0 = 10372;    // RB_COLOR_INFO shadow of render target 0.
  inline constexpr uint32_t kDepthInfo = 10376;     // RB_DEPTH_INFO shadow.
  inline constexpr uint32_t kTilingFlags = 11068;   // Bit 0: predicated tiling (Draw's fixup path).
  inline constexpr uint32_t kViewport = 13024;      // Floats x, y, width, height, min z, max z.
  inline constexpr uint32_t kPixelShader = 13068, kVertexShader = 13072;

  inline uint32_t Be32(const uint8_t *p)
  {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  inline uint16_t Be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
  inline float BeF32(const uint8_t *p) { return std::bit_cast<float>(Be32(p)); }
  inline void PutBe32(uint8_t *p, uint32_t value)
  {
    for (unsigned i = 0; i < 4; ++i)
      p[i] = uint8_t(value >> (24 - 8 * i));
  }

  // Host-order device state the producer checks.
  struct DeviceSnapshot
  {
    bool tiling = false;
    uint32_t color_info = 0, depth_info = 0, vertex_shader = 0, pixel_shader = 0;
    // Viewport in whole guest pixels (false: fractional or invalid).
    bool viewport_valid = false;
    std::array<uint32_t, 4> viewport{};  // x, y, width, height.
    uint32_t Area() const { return viewport[2] * viewport[3]; }
    static bool HdrColor(uint32_t info)
    {
      // RB_COLOR_INFO format (bits 16-19): 2_10_10_10_FLOAT (3, 12 as 16_16_16_16), 16_16_16_16 (5), 16_16_FLOAT (6), 16_16_16_16_FLOAT (7).
      const uint32_t format = (info >> 16) & 15;
      return format == 3 || format == 12 || format == 5 || format == 6 || format == 7;
    }
  };
  inline bool SnapshotDevice(std::span<const uint8_t> bytes, DeviceSnapshot &out)
  {
    if (bytes.size() < kDeviceBytes)
      return false;
    out.tiling = bytes[kTilingFlags] & 1;
    out.color_info = Be32(bytes.data() + kColorInfo0);
    out.depth_info = Be32(bytes.data() + kDepthInfo);
    out.vertex_shader = Be32(bytes.data() + kVertexShader);
    out.pixel_shader = Be32(bytes.data() + kPixelShader);
    out.viewport_valid = true;
    for (uint32_t i = 0; i < 4; ++i)
    {
      const float value = BeF32(bytes.data() + kViewport + 4 * i);
      if (!std::isfinite(value) || value < 0 || value > 16384 || value != std::floor(value))
      {
        out.viewport_valid = false;
        out.viewport[i] = 0;
        continue;
      }
      out.viewport[i] = uint32_t(value);
    }
    if (!out.viewport[2] || !out.viewport[3])
      out.viewport_valid = false;
    return true;
  }
} // namespace fuse::engine
