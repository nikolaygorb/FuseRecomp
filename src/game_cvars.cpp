#include "game_cvars.h"

REXCVAR_DEFINE_STRING(graphics_backend, "any", "GPU",
                      "Graphics API backend: any, d3d12, vulkan")
    .allowed({"any", "d3d12", "vulkan"});

REXCVAR_DEFINE_BOOL(dev_debug_runtime, false, "Debug",
                    "Enable runtime debug tools (stub sweep, missing function scan)");

REXCVAR_DEFINE_BOOL(fuse_skip_intro, false, "Gameplay",
                    "Skip intro videos");

REXCVAR_DEFINE_BOOL(fuse_texture_streaming_fix, false, "Performance",
                    "Texture Streaming Fix: disables the running RAM compactor tied to texture "
                    "streaming. Could theoretically cause stalling or crashing.");

REXCVAR_DEFINE_BOOL(fuse_fps60, false, "Gameplay",
                    "60 FPS. Physics stays at 30 FPS since lowering the fixed timestep breaks things.");

REXCVAR_DEFINE_BOOL(fuse_mount_cache, true, "Storage",
                    "Back the game's utility-drive cache (cache:, streaming cache) with a host "
                    "folder under the cache root, like xenia-canary's mount_cache");

REXCVAR_DEFINE_INT32(fuse_gpu_wait_mode, 1, "Performance",
                     "Pause between polls while the game waits for free command-buffer space: "
                     "0 = busy spin (original), 1 = yield the core, 2 = sleep 200us")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
