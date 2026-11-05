# Settings

Two checked-in config files loaded automatically at startup:

| File | Covers |
|---|---|
| [`hardware.toml`](hardware.toml) | Renderer, window, vsync, resolution, debug, gameplay patches, split-screen players |
| [`mapping.toml`](mapping.toml) | Input backend, keyboard bindings |

Flat `key = value` only — no `[Table]` headers (the loader reads root-level keys only).

Precedence: `--flag=value` > `REX_FLAG=value` > `settings/*.toml` > compiled default.

## `hardware.toml`

**In file** is what the checked-in `hardware.toml` sets (*not set* = the key is absent or commented out, so the default applies); **Default** is the compiled value used when the key is missing. Every SDK graphics option, including the ones not listed here, is described in [`faq.md`](faq.md) (a copy of the SDK's [`graphics-options.md`](../thirdparty/rexglue-sdk/docs/graphics-options.md)).

### Backend and image quality

| Key | In file | Default | Effect |
|---|---|---|---|
| `gpu_plugin` | `"xenos"` | `""` | GPU emulation plugin; empty disables GPU emulation. Leave as is |
| `graphics_backend` | `"any"` | `"any"` | `"any"` (D3D12 first, then Vulkan), `"d3d12"`, `"vulkan"`, `"d3d11"` (experimental, Windows; only by name, `"any"` never picks it — see [Direct3D 11](#direct3d-11-experimental)) |
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
| `async_depth_only_pipelines` | `true` | `false` | With `async_shader_compilation`, also prepares depth-only pipelines (shadows, depth pre-passes) in the background: no rare long pauses, but shadows or objects can be missing until ready |
| `store_shaders` | `true` | `true` | Keeps compiled shaders on disk, so later runs stutter less |

`resolution_scale` and `present_effect` combine: render at 2x, then `fsr` / `nis` / `cas` to the window size.

### Accuracy

| Key | In file | Default | Effect |
|---|---|---|---|
| `texture_cache_memory_limit_soft` | `1024` | `384` | MB of GPU memory for textures before old ones are dropped. Raised for `resolution_scale = 2` |
| `texture_cache_memory_limit_hard` | `4096` | `768` | MB at which dropping is forced (`128`–`8192`) |
| `clear_memory_page_state` | not set | `false` | Rechecks which memory the GPU wrote each frame. `true` can fix stale textures or missing models, but re-uploads that memory every frame (CPU/GPU cost) |
| `shared_memory_skip_unchanged_uploads` | `true` | `false` | D3D12 and Vulkan. Hashes each 4 KB page sent to the GPU and skips re-sending pages whose bytes didn't change. Mostly saves work together with `clear_memory_page_state = true`; costs ~1 ms CPU per 20 MB sent |
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
| `vulkan_allow_present_mode_fifo_latest_ready` | `true` | `false` | Where plain FIFO would be used, shows the newest finished frame each refresh (if the driver supports it): smoother, lower latency |
| `vulkan_async_skip_incomplete_frames` | `false` | `true` | With async shaders, hides frames where draws were skipped. Off here: such frames are shown |
| `vulkan_hoist_shared_memory_uploads` | `true` | `false` | Uploads vertex/index data rewritten mid-frame before the current batch of draws instead of interrupting it. Turn off if geometry flickers |
| `vulkan_max_frames_in_flight` | `2` | `0` (= 3) | Frames the CPU may prepare ahead of the GPU (`1`–`3`). `2` lowers input latency and `readback_resolve` waits |
| `vulkan_resolve_into_textures` | `true` | `false` | At `resolution_scale = 1` with `fbo`, copies render-to-texture results straight into the texture. No effect at higher scales. Turn off if colors look wrong |
| `vulkan_log_debug_messages` | `false` | `true` | Logs validation layer messages (only when the layers are on) |

### Direct3D 11 (experimental)

Windows only, built in (`REXGLUE_USE_D3D11=ON` in [`CMakeLists.txt`](../CMakeLists.txt)). Selected only with `graphics_backend = "d3d11"`. Meant for GPUs without Direct3D 12 or Vulkan; slower and less accurate than those. Needs feature level 11.0 (11.1 recommended). Limits and all `d3d11_*` options: [`faq.md`](faq.md#direct3d-11-experimental).

| Key | In file | Default | Effect |
|---|---|---|---|
| `d3d11_adapter` | `-1` | `-1` | GPU by DXGI adapter index; `-1` auto, `-2` WARP (software) |
| `d3d11_block_compressed_textures` | `true` | `true` | Keeps DXT/DXN textures compressed in video memory when their size allows. `false` decompresses all (more VRAM) |
| `d3d11_resolve_into_textures` | `false` | `false` | Same as `vulkan_resolve_into_textures`, for D3D11 |
| `d3d11_debug` | `false` | `false` | D3D11 debug layer (needs the Graphics Tools Windows feature). Debugging only |

### Misc and debug

| Key | In file | Default | Effect |
|---|---|---|---|
| `headless` | `false` | `false` | No UI; prompts use defaults |
| `log_level` | `"error"` | `"info"` | `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `perf_log_csv` | commented | `""` | Per-frame metrics CSV path. Only in builds with `REXGLUE_ENABLE_PERF_COUNTERS` (non-Release) |
| `dev_debug_runtime` | `false` | `false` | Runtime debug tools (stub sweep, missing function scan) → `logs/missed_functions.txt`, `logs/stub_sweep.txt` |

### Fuse

| Key | In file | Default | Effect |
|---|---|---|---|
| `fuse_skip_intro` | `false` | `false` | Ported xenia-canary "Skip intro" patch |
| `fuse_texture_streaming_fix` | `true` | `false` | Ported "Texture Streaming Fix": disables the RAM compactor tied to texture streaming (may stall/crash) |
| `fuse_mount_cache` | `true` | `true` | Back the game's `cache:` utility drive (streaming cache, ~800 MB) with `<cache root>/cache1`, like xenia-canary's `mount_cache` |
| `fuse_fps60` | `true` | `false` | Ported "60 FPS" patch, plus the in-level frame lock set to one vblank per frame instead of two, so levels run at 60 FPS with the normal 60 Hz vblank (leave `guest_vblank_rate` at 0: at 120 animations and rate of fire run twice as fast). Physics stays at 30 FPS. Read at startup and when a level loads |
| `fuse_gpu_wait_mode` | `1` | `1` | Render thread wait for the GPU: `0` busy spin (original), `1` yield, `2` sleep 200 µs |
| `user_language` | `1` | `1` | Game language: `1` English, `3` German, `4` French, `5` Spanish. SDK cvar; the game's static `XGetLanguage` reads it through `XCONFIG_USER_LANGUAGE`, and [`src/language_hook.cpp`](../src/language_hook.cpp) forces any language not on the disc back to English. Restart after changing it |

### Split-screen (local players)

The runtime can sign in up to four local players, as a console does when a second person picks up a controller. The game decides how split-screen looks and when it accepts a second player (menu or "press START to join" screen); the runtime only tells it who is signed in and which controller belongs to whom. Full guide, device assignment and examples: [`split-screen.md`](split-screen.md).

| Key | In file | Default | Effect |
|---|---|---|---|
| `max_local_users` | `4` | `1` | Local players that can be signed in (`1`–`4`). Player 2+ signs in while a controller is assigned to them and signs out when it goes away. `1` = single player |
| `keyboard_own_player` | not set (commented `true`) | `false` | Keyboard and mouse are player 1 on their own, gamepads start at player 2 (pad 1 = player 2). With `false`, keyboard + pad 1 are player 1 and pad 2 is player 2. Ignored once `input_player1` / `input_player2` is set |
| `input_player1` / `input_player2` | not set | `auto` | Explicit devices for players 1 and 2: comma-separated `keyboard`, `pad1`–`pad4`, `pads`, `none`, `auto` (e.g. `input_player1 = "pad1"`, `input_player2 = "keyboard"`). Players 3–4 exist in automatic mode only |
| `user_gamertag_2` | not set | `""` | Name shown for player 2 (max 15 bytes); empty gives `User(2)` |

With the checked-in values (`keyboard_own_player` left at `false`), keyboard + mouse and the first gamepad are player 1, and each further gamepad signs in the next player as soon as it is connected (up to 4). For one player on keyboard and mouse and the other on a gamepad, uncomment `keyboard_own_player = true`. Each player keeps their own in-game options (`profile/User2/` …), but saves are shared under player 1. [`fuse_mouse_look`](#mappingtoml) applies to player 1 only.

## `mapping.toml`

| Key | In file | Default | Effect |
|---|---|---|---|
| `input_backend` | `"sdl"` | `"sdl"` | `"sdl"`, `"xinput"`, or `"nop"` |
| `hid_mappings_file` | `"gamecontrollerdb.txt"` | `"gamecontrollerdb.txt"` | Extra SDL gamepad mappings, or `""` to skip |
| `guide_button` | `true` | `false` | Route Guide/PS button to guest |
| `mnk_mode` | `true` | `false` | Keyboard-as-controller, merged with any physical pad |
| `mnk_mouse` | `true` | `false` | Route mouse to right stick |
| `mnk_sensitivity` | `2.0` | `1.0` | Mouse sensitivity (`0.01`–`10.0`) for the emulated right stick |
| `fuse_mouse_look` | `true` | `true` | Native mouse look: the mouse turns the camera directly on foot (aiming or not) and on mounted turrets (no stick dead zone, curve or acceleration). Elsewhere it stays the emulated right stick. Player 1 only. Needs `mnk_mode` and `mnk_mouse` |
| `fuse_mouse_sensitivity` | `2.0` | `2.0` | Camera turn per mouse count = 0.022° × this (`0.01`–`100.0`) |
| `fuse_mouse_invert_x` / `fuse_mouse_invert_y` | `false` | `false` | Invert the mouse look axes |

Keyboard bindings (`keybind_a`, `keybind_lstick_up`, ...) are WASD-style. The file differs from the SDK defaults: X = `Q`, Y = `E` (SDK: `L`, `P`), left trigger = `I,RMB`, right trigger = `O,LMB` (SDK: `Q,I`, `E,O`). Rebind any of them the same way.

## Caveats

- The in-game Settings overlay's "Save to config" writes to `hardware.toml` — `git checkout -- settings/hardware.toml` reverts it.
- Engine hotkeys (console/debug/achievements toggles) are registered after these files load — rebind them from the in-game overlay instead.
