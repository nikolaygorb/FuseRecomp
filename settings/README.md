# Settings

Two checked-in config files loaded automatically at startup:

| File | Covers |
|---|---|
| [`hardware.toml`](hardware.toml) | Renderer, window, vsync, resolution, debug, gameplay patches |
| [`mapping.toml`](mapping.toml) | Input backend, keyboard bindings |

Flat `key = value` only — no `[Table]` headers (the loader reads root-level keys only).

Precedence: `--flag=value` > `REX_FLAG=value` > `settings/*.toml` > compiled default.

## `hardware.toml`

**In file** is what the checked-in `hardware.toml` sets; **Default** is the compiled value used when the key is missing. Every SDK graphics option, including the ones not listed here, is described in [`graphics-options.md`](../thirdparty/rexglue-sdk/docs/graphics-options.md).

### Backend and image quality

| Key | In file | Default | Effect |
|---|---|---|---|
| `gpu_plugin` | `"xenos"` | `""` | GPU emulation plugin; empty disables GPU emulation. Leave as is |
| `graphics_backend` | `"any"` | `"any"` | `"any"` (D3D12 first, then Vulkan), `"d3d12"`, `"vulkan"` |
| `vsync` | `true` | `true` | Paces the game to the display. Fuse ties its speed to it — `false` can make it run too fast |
| `resolution_scale` | `2` | `1` | Renders at a multiple of the original resolution (`1`–`8`). Best image quality, high GPU/memory cost. Can be changed while the game runs |
| `present_effect` | `"bilinear"` | `"bilinear"` | Scaling to the window: `"bilinear"`, `"cas"` (sharpening, up to 2x), `"fsr"` (FSR 1 upscale + sharpen), `"nis"` (NVIDIA Image Scaling, Vulkan only; D3D12 uses FSR's upscaler instead) |
| `present_fsr_sharpness_reduction` | `0.3` | `0.2` | FSR sharpening, in stops (`0.2`–`2.0`): higher is softer. Only with `present_effect = "fsr"` |
| `present_fsr_max_upsampling_passes` | commented | `4` | Limit of chained FSR passes (`1`–`4`, each up to 2x) |
| `present_cas_additional_sharpness` | commented | `0.0` | Extra CAS sharpness (`0.0`–`1.0`). Only with `present_effect = "cas"` |
| `present_fsr_quality_mode` | commented | — | Only for `fsr2` / `fsr3`, which exist only in builds with `REXGLUE_ENABLE_FIDELITYFX=ON` (off here) |
| `present_dither` | `true` | `false` | Dithers the final image; removes banding in skies and gradients |
| `swap_post_effect` | `"none"` | `"none"` | Anti-aliasing: `"none"`, `"fxaa"`, `"fxaa_extreme"`, `"smaa"` (SMAA 1x, sharper, a bit more GPU time) |
| `anisotropic_override` | `5` | `3` | Forced anisotropic filtering: `-1` leave to the game, `0` off, `1`–`5` = 1x–16x (`3` = 4x) |
| `native_2x_msaa` | `false` | `true` | GPU's native 2x MSAA for games that use 2x MSAA |
| `async_shader_compilation` | `true` | `true` | Compiles shaders in background threads: less stutter, objects can be missing for a moment the first time |
| `store_shaders` | `true` | `true` | Keeps compiled shaders on disk, so later runs stutter less |

`resolution_scale` and `present_effect` combine: render at 2x, then `fsr` / `nis` / `cas` to the window size.

### Accuracy

| Key | In file | Default | Effect |
|---|---|---|---|
| `texture_cache_memory_limit_soft` | `1024` | `384` | MB of GPU memory for textures before old ones are dropped. Raised for `resolution_scale = 2` |
| `texture_cache_memory_limit_hard` | `4096` | `768` | MB at which dropping is forced (`128`–`8192`) |
| `clear_memory_page_state` | `true` | `true` | Rechecks which memory the GPU wrote each frame. `false` saves a little CPU but can leave stale textures |
| `readback_memexport` | `true` | `true` | Copies GPU memory-export writes back to the CPU. **Fuse needs it** (distant textures) |
| `readback_memexport_fast` | `false` | `true` | Faster double-buffered copy for `readback_memexport`. Off here: the exact copy is used |
| `readback_resolve` | `"full"` | `"none"` | Copies render-to-texture results back to the CPU: `"none"`, `"fast"` / `"some"` (one frame late, cheap), `"full"` (exact, causes stalls) |
| `gpu_allow_invalid_fetch_constants` | `true` | `false` | Draws even with broken vertex/texture descriptors instead of skipping the draw. Can bring back missing objects, can also show garbage |
| `depth_bias_shader_offset` | `true` | `false` | Depth bias for decals in the shader. Fixes flickering decals and shadows (Z-fighting) |

### Window and display

| Key | In file | Default | Effect |
|---|---|---|---|
| `resolution` | `"1920x1080"` | `""` | Console video mode the game sees and the window size: `720p`, `1080p`, `1440p`, `4k` or `WIDTHxHEIGHT`. Render resolution is `resolution_scale` |
| `window_width` / `window_height` | `1920` / `1080` | `0` (title's own) | Window size in logical pixels; overrides the size from `resolution` |
| `fullscreen` | `true` | `true` | Start in fullscreen (borderless) |
| `monitor` | `0` | `0` | Monitor to open on: `0` default, `1` primary, `2` second, … |
| `present_letterbox` | `true` | `true` | Keeps the aspect ratio with black bars; `false` stretches |
| `host_present_from_non_ui_thread` | `true` | `true` | GPU thread presents frames itself. Turn off if the window freezes or flickers |

### Direct3D 12

| Key | In file | Default | Effect |
|---|---|---|---|
| `render_target_path_d3d12` | `"rtv"` | `""` (auto) | EDRAM emulation: `"rtv"` fast host render targets, `"rov"` more accurate, much slower |
| `d3d12_adapter` | `-1` | `-1` | GPU by DXGI adapter index; `-1` auto, `-2` WARP (software) |
| `d3d12_allow_variable_refresh_rate_and_tearing` | `true` | `true` | G-Sync/FreeSync and tearing when `vsync = false` |
| `d3d12_bindless` | `true` | `true` | Bindless descriptors. Turn off only to test a driver problem |
| `d3d12_pipeline_creation_threads` | `-1` | `-1` | Background pipeline threads (`-1` auto, up to `32`) |
| `d3d12_queue_priority` | `2` | `1` | GPU queue priority: `0` normal, `1` high, `2` realtime (may need administrator rights) |
| `d3d12_submit_on_primary_buffer_end` | `true` | `true` | Submits work each time a game command buffer ends, not only at frame end |
| `d3d12_debug`, `d3d12_break_on_error`, `d3d12_break_on_warning`, `d3d12_dxbc_disasm`, `d3d12_dxbc_disasm_dxilconv`, `d3d12_tessellation_wireframe` | `false` | `false` | Debugging only (debug layer, breaks, shader disassembly, wireframe). Slow — leave off |

### Vulkan

| Key | In file | Default | Effect |
|---|---|---|---|
| `render_target_path_vulkan` | `"fbo"` | `""` (auto = `fbo`) | `"fbo"` fast host render targets, `"fsi"` more accurate, slower, needs GPU support |
| `vulkan_allow_present_mode_immediate` | `false` | `true` | Off: no tearing with `vsync = true` (Vulkan otherwise uses immediate mode even with vsync) |
| `vulkan_allow_present_mode_mailbox` | `true` | `true` | Mailbox: low latency without tearing |
| `vulkan_allow_present_mode_fifo_relaxed` | `false` | `true` | Off: no tearing when frames are late (falls back to plain FIFO) |
| `vulkan_async_skip_incomplete_frames` | `false` | `true` | With async shaders, hides frames where draws were skipped. Off here: such frames are shown |
| `vulkan_force_convert_quad_lists_to_triangle_lists` | `true` | `false` | Vertex-shader fallback for quads even when geometry shaders exist |
| `vulkan_log_debug_messages` | `false` | `true` | Logs validation layer messages (only when the layers are on) |

### Misc and debug

| Key | In file | Default | Effect |
|---|---|---|---|
| `headless` | `false` | `false` | No UI; prompts use defaults |
| `log_level` | `"error"` | `"info"` | `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `perf_log_csv` | commented | `""` | Per-frame metrics CSV path. Only in builds with `REXGLUE_ENABLE_PERF_COUNTERS` (non-Release) |
| `dev_debug_runtime` | `false` | `false` | Runtime debug tools (stub sweep, missing function scan) → `logs/missed_functions.txt`, `logs/stub_sweep.txt` |
| `use_dedicated_xma_thread` | `false` | — | Not defined by this build (no such cvar in the SDK or `src/`); has no effect |

### Fuse

| Key | In file | Default | Effect |
|---|---|---|---|
| `fuse_skip_intro` | `true` | `false` | Ported xenia-canary "Skip intro" patch |
| `fuse_texture_streaming_fix` | `true` | `false` | Ported "Texture Streaming Fix": disables the RAM compactor tied to texture streaming (may stall/crash) |
| `fuse_mount_cache` | `true` | `true` | Back the game's `cache:` utility drive (streaming cache, ~800 MB) with `<cache root>/cache1`, like xenia-canary's `mount_cache` |
| `fuse_fps60` | `true` | `false` | Ported "60 FPS" patch; physics stays at 30 FPS |
| `fuse_gpu_wait_mode` | `1` | `1` | Render thread wait for the GPU: `0` busy spin (original), `1` yield, `2` sleep 200 µs |
| `user_language` | `1` | `1` | Game language: `1` English, `3` German, `4` French, `5` Spanish. SDK cvar; the game's static `XGetLanguage` reads it through `XCONFIG_USER_LANGUAGE`, and [`src/language_hook.cpp`](../src/language_hook.cpp) forces any language not on the disc back to English. Restart after changing it |

## `mapping.toml`

| Key | Default | Effect |
|---|---|---|
| `input_backend` | `"sdl"` | `"sdl"`, `"xinput"`, or `"nop"` |
| `hid_mappings_file` | `"gamecontrollerdb.txt"` | Extra SDL gamepad mappings, or `""` to skip |
| `guide_button` | `true` | Route Guide/PS button to guest |
| `mnk_mode` | `true` | Keyboard-as-controller, merged with any physical pad |
| `mnk_mouse` | `true` | Route mouse to right stick |
| `mnk_sensitivity` | `1.0` | Mouse sensitivity (`0.01`–`10.0`) for the emulated right stick |
| `fuse_mouse_look` | `true` | Native mouse look: the mouse turns the camera directly on foot (aiming or not) and on mounted turrets (no stick dead zone, curve or acceleration). Elsewhere it stays the emulated right stick. Player 1 only. Needs `mnk_mode` and `mnk_mouse` |
| `fuse_mouse_sensitivity` | `2.0` | Camera turn per mouse count = 0.022° × this (`0.01`–`100.0`) |
| `fuse_mouse_invert_x` / `fuse_mouse_invert_y` | `false` | Invert the mouse look axes |

Keyboard bindings (`keybind_a`, `keybind_lstick_up`, ...) have sensible WASD-style defaults in the file. Rebind any of them the same way.

## Caveats

- The in-game Settings overlay's "Save to config" writes to `hardware.toml` — `git checkout -- settings/hardware.toml` reverts it.
- Engine hotkeys (console/debug/achievements toggles) are registered after these files load — rebind them from the in-game overlay instead.
