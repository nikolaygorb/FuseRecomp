// game_cvars.h - Game-specific CVAR declarations
#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, graphics_backend);
REXCVAR_DECLARE(bool, dev_debug_runtime);
REXCVAR_DECLARE(bool, fuse_skip_intro);
REXCVAR_DECLARE(bool, fuse_texture_streaming_fix);
REXCVAR_DECLARE(bool, fuse_fps60);
REXCVAR_DECLARE(int32_t, fuse_gpu_wait_mode);
REXCVAR_DECLARE(bool, fuse_mount_cache);
