#include <array>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "game_cvars.h"
#include "game_constants.h"
#include "game_patches.h"

namespace
{
    // Writes through the guest heap's own Protect()/TranslateVirtual(), rather
    // than raw host-pointer arithmetic + rex::memory::Protect(). Some platforms
    // (Linux) don't support combined writable+executable pages, so the guest
    // heap keeps a separate write-view mapping for code pages; writing to
    // `virtual_membase() + address` directly bypasses that view and segfaults.
    bool WriteGuestBytes(uint32_t address, const uint8_t *bytes, uint32_t size)
    {
        if (!bytes || !size)
        {
            REXLOG_WARN("Cannot apply game patch at guest address {:08X}: empty patch data", address);
            return false;
        }

        auto *rt = rex::Runtime::instance();
        auto *memory = rt ? rt->memory() : nullptr;
        auto *heap = memory ? memory->LookupHeap(address) : nullptr;
        if (!heap)
        {
            REXLOG_WARN("Cannot apply game patch at guest address {:08X}: no guest heap", address);
            return false;
        }

        const uint64_t last_address = static_cast<uint64_t>(address) + size - 1;
        if (last_address > UINT32_MAX ||
            address / heap->page_size() != last_address / heap->page_size())
        {
            REXLOG_WARN("Cannot apply game patch at guest address {:08X}: range crosses a guest page",
                        address);
            return false;
        }

        constexpr uint32_t writable = rex::memory::kMemoryProtectRead |
                                      rex::memory::kMemoryProtectWrite;
        uint32_t old_protect{};
        if (!heap->Protect(address, size, writable, &old_protect))
        {
            REXLOG_WARN("Cannot make guest range {:08X}+{:X} writable for game patch", address, size);
            return false;
        }

        // Log old -> new so a wrong game version shows up as an unexpected
        // original value in the log.
        auto *dest = memory->TranslateVirtual(address);
        uint32_t old_value{};
        uint32_t new_value{};
        for (uint32_t i = 0; i < size; ++i)
        {
            old_value = (old_value << 8) | dest[i];
            new_value = (new_value << 8) | bytes[i];
        }
        std::memcpy(dest, bytes, size);
        if (!heap->Protect(address, size, old_protect, nullptr))
        {
            REXLOG_WARN("Cannot restore guest protection for game patch at {:08X}", address);
            return false;
        }
        REXLOG_INFO("Game patch at {:08X}: {:0{}X} -> {:0{}X}", address, old_value, size * 2,
                    new_value, size * 2);
        return true;
    }

    std::array<uint8_t, 4> Be32Bytes(uint32_t value)
    {
        return {
            static_cast<uint8_t>(value >> 24),
            static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value)};
    }

    // `patch.value` is right-aligned, so the low `width` bytes of its be32
    // form are the bytes to write regardless of patch width (1/2/4).
    bool WritePatch(const GameConstants::PatchConstants::Patch &patch)
    {
        const auto bytes = Be32Bytes(patch.value);
        return WriteGuestBytes(static_cast<uint32_t>(patch.address),
                               bytes.data() + (bytes.size() - patch.width), patch.width);
    }
} // namespace

// Ported from xenia-canary game-patches for Fuse (4541096E)
// https://github.com/xenia-canary/game-patches/blob/main/patches/4541096E%20-%20Fuse.patch.toml
//
// Only patches that write real data live here. Code patches are
// [[midasm_hook]]s in patches.toml, implemented in src/fuse_hooks.cpp.
namespace game_patches
{
    void ApplyEnabledPatches()
    {
        if (REXCVAR_GET(fuse_skip_intro))
        {
            WritePatch(GameConstants::PatchConstants::SkipIntro());
        }
        if (REXCVAR_GET(fuse_texture_streaming_fix))
        {
            WritePatch(GameConstants::PatchConstants::TextureStreamingFix());
        }
    }
} // namespace game_patches
