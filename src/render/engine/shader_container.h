#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "device_layout.h"

namespace fuse::engine
{
  struct ShaderConstant
  {
    std::string name;
    uint16_t register_set = 0, index = 0, count = 0;  // Set 2: float4.
  };
  struct ShaderContainer
  {
    bool pixel = false;
    uint32_t header_bytes = 0, code_bytes = 0;
    std::string name;  // Build name ("VS_Main_..."), empty if absent.
    std::vector<ShaderConstant> constants;
    // First float register of a constant, -1 if the shader does not read it.
    int Float(std::string_view constant) const
    {
      for (const auto &c : constants)
        if (c.register_set == 2 && c.name == constant)
          return c.index;
      return -1;
    }
  };

  // bytes: the container's header (header_bytes of it are enough).
  inline bool ParseShaderContainer(const uint8_t *bytes, uint32_t available, ShaderContainer &out)
  {
    if (available < 32)
      return false;
    const uint32_t magic = Be32(bytes);
    if ((magic >> 8) != 0x102A11 || (magic & 0xFE))
      return false;
    out.pixel = !(magic & 1);
    out.header_bytes = Be32(bytes + 4);
    out.code_bytes = Be32(bytes + 8);
    if (out.header_bytes < 32 || out.header_bytes > available || out.header_bytes > 65536 || !out.code_bytes ||
        out.code_bytes > 1024 * 1024)
      return false;
    const uint32_t limit = out.header_bytes;
    // Build name: the debug path's file name.
    const std::string_view header(reinterpret_cast<const char *>(bytes), limit);
    if (const size_t built = header.find("Built\\"); built != std::string_view::npos)
    {
      const size_t begin = built + 6, end = header.find(".updb", begin);
      if (end != std::string_view::npos && end - begin < 128)
        out.name.assign(header.substr(begin, end - begin));
    }
    const uint32_t table = Be32(bytes + 16) + 4;
    if (uint64_t(table) + 28 > limit)
      return true;  // No constant table: name only.
    const uint32_t version = Be32(bytes + table + 8);
    if (version != 0xFFFE0300u && version != 0xFFFF0300u)
      return true;
    const uint32_t count = Be32(bytes + table + 12), info = Be32(bytes + table + 16);
    if (count > 256 || uint64_t(table) + info + uint64_t(count) * 20 > limit)
      return true;
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint8_t *entry = bytes + table + info + i * 20;
      ShaderConstant constant;
      constant.register_set = Be16(entry + 4);
      constant.index = Be16(entry + 6);
      constant.count = Be16(entry + 8);
      const uint32_t name = table + Be32(entry);
      for (uint32_t p = name; p < limit && bytes[p] && constant.name.size() < 96; ++p)
        constant.name.push_back(char(bytes[p]));
      out.constants.push_back(std::move(constant));
    }
    return true;
  }
} // namespace fuse::engine
