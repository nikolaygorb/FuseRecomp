#include "scene_probe.h"
#include "device_layout.h"
#include "scene_camera.h"
#include "shader_container.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/graphics/title_scene.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>

REXCVAR_DEFINE_BOOL(fuse_engine_scene_stream, false, "Render/Engine",
                    "Emit scene markers every frame for the Native scene consumer. On by itself while the SDK's "
                    "consumer is (native_scene_frames, native_temporal_upscaler, native_frame_generation); this forces it "
                    "for diagnostics.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(fuse_engine_scene_probe, false, "Render/Engine",
                    "Capture series only: an SDK scene probe (target export) before every scene-sized colour resolve, "
                    "to see where the shaded scene is complete. Needs fuse_engine_report_dir.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fuse_engine_capture_request, 0, "Render/Engine",
                     "Scene capture: change to a new nonzero number (F4) to capture a series after "
                     "fuse_engine_capture_delay_frames; the SDK exports its frames into fuse_engine_report_dir.")
    .range(0, INT32_MAX)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fuse_engine_capture_frames, 1, "Render/Engine", "Scene capture: consecutive frames per request.")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fuse_engine_capture_delay_frames, 0, "Render/Engine",
                     "Scene capture: frames to wait after a request (time to close F4 and move the camera).")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fuse_engine_scene_end_caller, "", "Render/Engine",
                      "Scene stream: where the scene ends, the return address (hex) of the title's call of its resolve "
                      "wrapper (sub_82883D30), optionally \"/n\" for its n-th call in the frame (default 1). The scene end "
                      "(and the temporal upscaler's output) goes right before that resolve. Empty: learned (the last "
                      "resolve of the view's colour target before post-processing); scene-producer.json shows it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(fuse_engine_scene_cut_distance, 2.0, "Render/Engine",
                      "Scene stream: a camera position jump above this (world units, metres, per frame) resets temporal "
                      "history.")
    .range(0.05, 100000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(fuse_engine_scene_meters_per_unit, 1.0, "Render/Engine",
                      "Scene stream: world units in metres (FSR and DLSS scale hints; Insomniac's engine uses metres).")
    .range(0.0001, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(fuse_engine_scene_auto_opaque, false, "Render/Engine",
                    "Scene stream: ask the SDK to snapshot opaque colour before the first blended scene draw (reactive "
                    "mask). Off: Fuse's light pre-pass blends its lights first, which is not the opaque boundary.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(fuse_engine_scene_view_hdr, true, "Render/Engine",
                    "Scene stream: the view is a camera draw into an HDR colour target (the shaded scene; the light "
                    "pre-pass G-buffer is 8-bit), so the view's target is the same every frame. Off: any colour target.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fuse_engine_trace_interval, 0, "Render/Engine",
                     "Diagnostics: every this many frames, trace fuse_engine_trace_frames frames (every draw, resolve and "
                     "marker) into fuse_engine_report_dir/trace.jsonl. 0: off.")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fuse_engine_trace_frames, 2, "Render/Engine", "Diagnostics: consecutive frames per trace.")
    .range(1, 16)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace fuse::engine::scene
{
  namespace
  {
    using rex::graphics::title::ScenePoint;
    // Fuse launch facts (constant tables of the XEX's shaders, November 2026).
    constexpr uint32_t kWorldToClipRegister = 68;  // g_VP_WorldToClipMat, every shader that has it.
    constexpr uint32_t kCameraPositionRegister = 72;  // g_VP_WSCamPos.
    constexpr uint32_t kConstantRegisters = 256;      // VS float constants c0..c255.
    // The main view: a large viewport (split-screen halves too, see below).
    constexpr uint32_t kMinViewArea = 720 * 405;
    // First pixel shaders of the post chain (after the shaded scene).
    constexpr std::string_view kPostShaders[] = {"PS_BrightPass", "PS_DepthOfField", "PS_RadialBlur", "PS_ToneMap",
                                                 "PS_Fxaa"};
    enum ResetReason : uint32_t
    {
      kResetNone,
      kResetTeleport
    };
    enum ViewSource : uint32_t
    {
      kSourceRegister,  // The VS reads g_VP_WorldToClipMat.
      kSourceC68,       // c68, checked against the draw's object matrices.
      kSourceObject,    // clip * world^-1 of the draw (c68 was stale).
      kSourceCount
    };
    constexpr const char *kSourceNames[kSourceCount] = {"register", "c68", "object"};

    struct Shader
    {
      std::string name;
      bool pixel = false, hud = false, post = false;
      uint64_t hash = 0;
      int world_to_clip = -1, object_to_clip = -1, object_to_world = -1;
      // The camera in its constants: c68 itself, the object's clip and world
      // matrices, or the object's clip matrix (then c68 when loaded this frame).
      bool Camera() const { return world_to_clip >= 0 || object_to_clip >= 0; }
      bool light_shell = false;  // Light volumes (the light buffer, before the shaded scene).
    };

    std::atomic<bool> enabled{false}, active{false}, capture_requested{false}, stream_changed{false};
    std::atomic<uint32_t> bound_vertex_shader{0}, bound_pixel_shader{0};  // OnBindShaders.
    std::atomic<bool> bound_seen{false};
    std::mutex mutex;
    rex::memory::Memory *memory = nullptr;
    rex::system::IGraphicsSystem *graphics = nullptr;
    std::filesystem::path directory, report_path, trace_path;
    uint64_t frame = 1, first = 0, end = 0, session = 0;
    bool stream = false;

    // Shaders by D3D object (a reused address takes the new shader).
    std::vector<Shader> shaders;
    std::unordered_map<uint32_t, uint32_t> shader_by_object;  // -> index into shaders.
    size_t traced_shaders = 0;

    // This frame.
    uint64_t marked_view = 0, scene_end_frame = 0, post_end_frame = 0, post_frame = 0;
    // kView attempts this frame: on the 1st, 8th and 32nd camera draw that has
    // the camera, and on each one until one is placed (a full ring segment);
    // a later one replaces an earlier one in the SDK, one the SDK refuses (a
    // draw it skipped) keeps the earlier.
    uint32_t camera_draws = 0;
    const char *frame_refusal = nullptr;  // The last camera draw's refusal (frames without a view).
    std::array<double, 3> frame_eye{};
    DeviceSnapshot view_state;
    uint32_t view_width = 0, view_height = 0;
    std::unordered_map<uint32_t, uint32_t> resolve_calls;  // Site -> calls this frame.
    uint64_t frame_candidate = 0;                           // Site << 8 | call: the latest scene-end candidate.
    std::vector<std::array<uint32_t, 4>> camera_viewports;  // Distinct viewports of camera draws.
    uint32_t draw_ordinal = 0, traced_camera_draws = 0;
    // Resolves learned by call (site << 8 | call): the same one three frames in
    // a row. Passes differ (gameplay, front-end 3D, depth of field: other
    // resolve sites), so several hold.
    struct Learner
    {
      std::vector<uint64_t> keys;
      uint64_t pending = 0;
      uint32_t count = 0;
      bool Has(uint64_t key) const { return std::find(keys.begin(), keys.end(), key) != keys.end(); }
      std::string Text() const
      {
        std::string text;
        for (const uint64_t key : keys)
          text += fmt::format("{}{:X}/{}", text.empty() ? "" : ",", key >> 8, key & 255);
        return text;
      }
      // True when the key is newly learned.
      bool Note(uint64_t key)
      {
        count = key == pending ? count + 1 : 1;
        pending = key;
        if (count < 3 || Has(key))
          return false;
        if (keys.size() >= 4)
          keys.erase(keys.begin());
        keys.push_back(key);
        return true;
      }
    };
    // Scene ends: each the last candidate of its frames; a frame ends at the first.
    Learner scene_ends;
    // Frame ends: the last scene-sized colour resolve after the post chain (the
    // front buffer's copy). kPostEnd before it when no HUD came first (gameplay
    // with its HUD hidden, cutscenes): at the swap the frame is already copied.
    Learner frame_ends;
    uint64_t frame_last_resolve = 0;
    // Markers that found the ring segment full (Queue appends only inside the
    // reserved space), placed again once the title's call has made room: the
    // scene end right after its resolve (the target is unchanged until the
    // next draw), the frame end after the swap (before the next frame's draws).
    uint64_t scene_end_retry = 0;
    DeviceSnapshot scene_end_retry_state;
    bool frame_end_retry = false;
    DeviceSnapshot frame_end_retry_state;
    bool split_screen = false;
    // Camera history.
    std::array<double, 3> previous_eye{};
    uint64_t previous_eye_frame = 0;
    std::chrono::steady_clock::time_point last_swap{};
    float frame_seconds = 0;
    // Trace frames [trace_first, trace_end).
    uint64_t trace_first = 0, trace_end = 0;
    std::string trace;
    // Ring scan (Scan below): the VS float constants as the GPU will see them.
    std::array<uint32_t, 1024> alu{};  // Host-order words, register 0x4000 + i (c0.x .. c255.w).
    uint32_t scan_cursor = 0;          // Last parsed word (the D3D cursor's convention).
    uint64_t scan_words = 0, scan_resyncs = 0, scan_partials = 0, scan_garbage = 0, alu_type0 = 0, alu_set = 0,
             alu_load = 0, scan_indirect = 0;

    // Report.
    uint64_t queued = 0, ring_skips = 0, invalid = 0, views = 0, skips_by_point[8]{}, views_by_source[kSourceCount]{};
    uint64_t view_refused_tiling = 0, view_refused_viewport = 0, view_refused_projection = 0, view_refused_matrix = 0,
             view_refused_target = 0;
    uint64_t c68_stale = 0, eye_mismatch = 0, split_frames = 0, frames_without_view = 0, resets_teleport = 0;
    uint64_t scene_end_at_resolve = 0, scene_end_missed = 0, post_end_hud = 0, post_end_resolve = 0, post_end_swap = 0;
    uint64_t scene_end_after_resolve = 0, frame_end_after_swap = 0;
    std::map<std::string, uint64_t> view_shaders;  // VS name -> kView markers.
    std::map<std::string, uint64_t> no_view_reasons;  // Frames without a view, by the last refusal.
    uint64_t view_retries = 0;
    std::map<uint32_t, uint64_t> refused_viewports;  // width << 16 | height -> count.

    uint8_t *Access(uint32_t address, uint32_t bytes, bool write = false)
    {
      if (!memory || !address || !bytes || uint64_t(address) + bytes > 0x100000000ull)
        return nullptr;
      const auto access = memory->QueryRangeAccess(address, bytes);
      if (access == rex::memory::PageAccess::kNoAccess || (write && !(uint32_t(access) & 2)))
        return nullptr;
      return memory->TranslateVirtual<uint8_t *>(address);
    }
    bool Series() { return first && frame >= first && frame < end; }
    bool Tracing() { return !trace_path.empty() && frame >= trace_first && frame < trace_end; }
    bool Producing() { return stream || Series(); }
    std::string Hex(uint64_t value)
    {
      char text[24];
      std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(value));
      return text;
    }
    // fuse_engine_scene_end_caller: "site[/call]" -> site << 8 | call; 0 = learned.
    uint64_t ConfiguredEnd()
    {
      const auto &text = REXCVAR_GET(fuse_engine_scene_end_caller);
      if (text.empty())
        return 0;
      char *rest = nullptr;
      const uint32_t site = uint32_t(std::strtoul(text.c_str(), &rest, 16));
      uint32_t call = 1;
      if (rest && *rest == '/')
        call = uint32_t(std::strtoul(rest + 1, nullptr, 10));
      return site ? uint64_t(site) << 8 | std::clamp<uint32_t>(call, 1, 255) : 0;
    }
    std::string EndText(uint64_t key) { return key ? Hex(key >> 8) + "/" + std::to_string(key & 255) : ""; }

    void Report()
    {
      if (report_path.empty())
        return;
      uint64_t stats[5]{};
      if (graphics)
        graphics->QueryTitleSceneStats(stats, 5);
      std::ofstream file(report_path);
      size_t vs = 0;
      for (const auto &s : shaders)
        vs += !s.pixel;
      file << "{\"schema\":1,\"session\":" << session << ",\"frame\":" << frame << ",\"queued\":" << queued
           << ",\"views\":" << views << ",\"views_register\":" << views_by_source[kSourceRegister]
           << ",\"views_c68\":" << views_by_source[kSourceC68] << ",\"views_object\":" << views_by_source[kSourceObject]
           << ",\"ring_skips\":" << ring_skips << ",\"skips_view\":" << skips_by_point[1]
           << ",\"skips_scene_end\":" << skips_by_point[2] << ",\"skips_post_end\":" << skips_by_point[3]
           << ",\"skips_frame_end\":" << skips_by_point[4] << ",\"invalid\":" << invalid
           << ",\"scene_end_learned\":\"" << scene_ends.Text() << "\",\"scene_end_configured\":\""
           << EndText(ConfiguredEnd()) << "\",\"scene_end_at_resolve\":" << scene_end_at_resolve
           << ",\"scene_end_missed\":" << scene_end_missed << ",\"scene_end_after_resolve\":" << scene_end_after_resolve
           << ",\"frame_end_after_swap\":" << frame_end_after_swap << ",\"frame_end_learned\":\"" << frame_ends.Text()
           << "\",\"post_end_hud\":" << post_end_hud << ",\"post_end_resolve\":" << post_end_resolve
           << ",\"post_end_swap\":" << post_end_swap << ",\"frames_without_view\":" << frames_without_view
           << ",\"split_frames\":" << split_frames << ",\"view_refused_tiling\":" << view_refused_tiling
           << ",\"view_refused_viewport\":" << view_refused_viewport
           << ",\"view_refused_projection\":" << view_refused_projection
           << ",\"view_refused_matrix\":" << view_refused_matrix << ",\"view_refused_target\":" << view_refused_target
           << ",\"c68_stale\":" << c68_stale << ",\"scan_words\":" << scan_words
           << ",\"scan_resyncs\":" << scan_resyncs << ",\"scan_partials\":" << scan_partials
           << ",\"scan_garbage\":" << scan_garbage << ",\"alu_type0\":" << alu_type0 << ",\"alu_set\":" << alu_set
           << ",\"alu_load\":" << alu_load << ",\"scan_indirect\":" << scan_indirect
           << ",\"eye_mismatch\":" << eye_mismatch << ",\"resets_teleport\":" << resets_teleport
           << ",\"view_width\":" << view_width << ",\"view_height\":" << view_height << ",\"shaders_vs\":" << vs
           << ",\"shaders_ps\":" << shaders.size() - vs << ",\"view_shaders\":{";
      bool first_entry = true;
      for (const auto &[name, count] : view_shaders)
      {
        file << (first_entry ? "" : ",") << "\"" << name << "\":" << count;
        first_entry = false;
      }
      file << "},\"no_view_reasons\":{";
      first_entry = true;
      for (const auto &[why, count] : no_view_reasons)
      {
        file << (first_entry ? "" : ",") << "\"" << why << "\":" << count;
        first_entry = false;
      }
      file << "},\"view_retries\":" << view_retries << ",\"refused_viewports\":{";
      first_entry = true;
      for (const auto &[size, count] : refused_viewports)
      {
        file << (first_entry ? "" : ",") << "\"" << (size >> 16) << 'x' << (size & 0xFFFF) << "\":" << count;
        first_entry = false;
      }
      file << "},\"sdk_consumed\":" << stats[0] << ",\"sdk_completed\":" << stats[1] << ",\"sdk_failed\":" << stats[2]
           << ",\"sdk_views_accepted\":" << stats[3] << "}\n";
      file.flush();
      if (!file)
        REXLOG_ERROR("Fuse scene: cannot write producer report {}", report_path.string());
    }

    // Trace (diagnostics): one JSON object per line.
    void TraceShaders()
    {
      for (; traced_shaders < shaders.size(); ++traced_shaders)
      {
        const auto &s = shaders[traced_shaders];
        trace += fmt::format("{{\"e\":\"shader\",\"id\":{},\"name\":\"{}\",\"ps\":{},\"hash\":\"{:016X}\",\"vp\":{},"
                             "\"clip\":{},\"world\":{},\"hud\":{},\"post\":{}}}\n",
                             traced_shaders, s.name, s.pixel, s.hash, s.world_to_clip, s.object_to_clip,
                             s.object_to_world, s.hud, s.post);
      }
    }
    int ShaderId(uint32_t object)
    {
      const auto found = shader_by_object.find(object);
      return found == shader_by_object.end() ? -1 : int(found->second);
    }
    std::string TraceState(const DeviceSnapshot &state)
    {
      return fmt::format("\"vs\":{},\"ps\":{},\"vp\":[{},{},{},{}],\"vp_ok\":{},\"tiling\":{},\"color\":\"{:08X}\","
                         "\"depth\":\"{:08X}\"",
                         ShaderId(state.vertex_shader), ShaderId(state.pixel_shader), state.viewport[0],
                         state.viewport[1], state.viewport[2], state.viewport[3], state.viewport_valid, state.tiling,
                         state.color_info, state.depth_info);
    }
    void FlushTrace()
    {
      if (trace.empty() || trace_path.empty())
        return;
      std::ofstream file(trace_path, std::ios::app);
      file << trace;
      if (!file)
        REXLOG_WARN("Fuse scene: cannot write trace {}", trace_path.string());
      trace.clear();
    }

    // The VS float constants as the GPU will see them. Fuse loads them past
    // D3D (its device copy stays zero): the producer reads the packets the
    // title writes into the ring between its D3D calls, the subset of the
    // command processor that sets ALU constants (type-0 writes of 0x4000+,
    // SET_CONSTANT, SET_CONSTANT2, SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT from
    // memory, INDIRECT_BUFFER).
    uint64_t c68_frame = 0;  // Frame c68-c71 were last loaded in (the engine loads what a shader reads).
    void WriteRegister(uint32_t reg, uint32_t value)
    {
      if (reg >= 0x4000 && reg < 0x4400)
      {
        alu[reg - 0x4000] = value;
        if (reg >= 0x4000 + kWorldToClipRegister * 4 && reg < 0x4000 + (kWorldToClipRegister + 4) * 4)
          c68_frame = frame;
      }
    }
    const uint8_t *Physical(uint32_t address, uint32_t bytes)
    {
      if (!memory || uint64_t(address) + bytes > 0x20000000ull)
        return nullptr;
      return memory->TranslatePhysical<uint8_t *>(address);
    }
    // Complete packets of [p, p + bytes); returns the bytes they take.
    size_t ParsePackets(const uint8_t *p, size_t bytes, int depth)
    {
      size_t at = 0;
      while (at + 4 <= bytes)
      {
        const uint32_t header = Be32(p + at);
        const uint32_t type = header >> 30, count = ((header >> 16) & 0x3FFF) + 1;
        size_t size = 4;
        if (type == 0 || type == 3)
          size = 4 + size_t(count) * 4;
        else if (type == 1)
          size = 12;
        if (size > 256 * 1024)
        {
          // Not a packet (reserved space the title has not filled): a word on.
          ++scan_garbage;
          at += 4;
          continue;
        }
        if (at + size > bytes)
          break;
        const uint8_t *data = p + at + 4;
        if (type == 0)
        {
          const uint32_t base = header & 0x7FFF;
          const bool one = (header >> 15) & 1;
          if (base < 0x4400 && base + (one ? 1 : count) > 0x4000)
          {
            ++alu_type0;
            for (uint32_t i = 0; i < count; ++i)
              WriteRegister(one ? base : base + i, Be32(data + i * 4));
          }
        }
        else if (type == 3)
        {
          switch ((header >> 8) & 0x7F)
          {
          case 0x2D:  // SET_CONSTANT
            if (const uint32_t offset_type = Be32(data); ((offset_type >> 16) & 0xFF) == 0)
            {
              ++alu_set;
              for (uint32_t i = 0; i + 1 < count; ++i)
                WriteRegister(0x4000 + (offset_type & 0x7FF) + i, Be32(data + 4 + i * 4));
            }
            break;
          case 0x55:  // SET_CONSTANT2
          case 0x56:  // SET_SHADER_CONSTANTS
            for (uint32_t i = 0; i + 1 < count; ++i)
              WriteRegister((Be32(data) & 0xFFFF) + i, Be32(data + 4 + i * 4));
            break;
          case 0x2F:  // LOAD_ALU_CONSTANT
            if (count >= 3)
            {
              const uint32_t address = Be32(data) & 0x3FFFFFFF, offset_type = Be32(data + 4);
              const uint32_t words = Be32(data + 8) & 0xFFF;
              if (((offset_type >> 16) & 0xFF) == 0)
                if (const auto *source = Physical(address, words * 4))
                {
                  ++alu_load;
                  for (uint32_t i = 0; i < words; ++i)
                    WriteRegister(0x4000 + (offset_type & 0x7FF) + i, Be32(source + i * 4));
                }
            }
            break;
          case 0x3F:  // INDIRECT_BUFFER
          case 0x37:  // INDIRECT_BUFFER_PFD
            if (count >= 2 && depth < 2)
            {
              const uint32_t words = Be32(data + 4) & 0xFFFFF;
              if (const auto *list = Physical(Be32(data) & 0x1FFFFFFF, words * 4))
              {
                ++scan_indirect;
                ParsePackets(list, size_t(words) * 4, depth + 1);
              }
            }
            break;
          default:
            break;
          }
        }
        at += size;
      }
      return at;
    }
    // The ring up to the device's cursor; a jump (a new segment, a stream
    // that just started) starts over there.
    void Scan(uint32_t device)
    {
      const auto *bytes = Access(device + kRingCursor, 4);
      if (!bytes)
        return;
      const uint32_t cursor = Be32(bytes);
      if (!scan_cursor || cursor < scan_cursor || cursor - scan_cursor > 8 * 1024 * 1024)
      {
        scan_cursor = cursor;
        ++scan_resyncs;
        return;
      }
      if (cursor == scan_cursor)
        return;
      const uint32_t length = cursor - scan_cursor;
      const auto *words = Access(scan_cursor + 4, length);
      if (!words)
      {
        scan_cursor = cursor;
        ++scan_resyncs;
        return;
      }
      const size_t used = ParsePackets(words, length, 0);
      scan_words += used / 4;
      if (used < length)
        ++scan_partials;  // The rest when it is complete.
      scan_cursor += uint32_t(used);
    }

    // A 4x4 matrix of four registers. Fuse's shaders multiply row vectors
    // (clip = p * M; the translation is the last register), so the column
    // vector matrix the contract uses is the registers transposed: checked in
    // game (c68 equals ObjToClip * ObjToWorld^-1, the camera position derived
    // from it equals g_VP_WSCamPos).
    camera::Matrix ReadMatrix(uint32_t base)
    {
      camera::Matrix m{};
      for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
          m[row * 4 + column] = std::bit_cast<float>(alu[(base + column) * 4 + row]);
      return m;
    }
    bool Finite(const camera::Matrix &m)
    {
      return std::all_of(m.begin(), m.end(), [](double v) { return std::isfinite(v); });
    }

    // The draw's camera; false: none (reason counted).
    struct ViewCamera
    {
      camera::Camera camera;
      ViewSource source = kSourceObject;
      uint32_t camera_register = rex::graphics::title::kNoRegister;
      std::array<double, 3> position{};  // g_VP_WSCamPos.
    };
    bool DrawCamera(const Shader &vs, ViewCamera &out, const char **reason)
    {
      *reason = "registers";
      const camera::Matrix c68 = ReadMatrix(kWorldToClipRegister);
      for (size_t i = 0; i < 3; ++i)
        out.position[i] = std::bit_cast<float>(alu[kCameraPositionRegister * 4 + i]);
      auto block = [](int index) { return index >= 0 && uint32_t(index) + 4 <= kConstantRegisters; };
      camera::Matrix pv{};
      if (block(vs.world_to_clip))
      {
        pv = ReadMatrix(uint32_t(vs.world_to_clip));
        out.source = kSourceRegister;
        // No camera_register: the SDK compares the registers with the matrix
        // rows, and Fuse's registers hold its columns.
      }
      else if (block(vs.object_to_clip) && block(vs.object_to_world))
      {
        const camera::Matrix clip = ReadMatrix(uint32_t(vs.object_to_clip));
        const camera::Matrix world = ReadMatrix(uint32_t(vs.object_to_world));
        *reason = "object-matrix";
        if (!Finite(clip) || !Finite(world) || !camera::FromObject(clip, world, pv))
          return false;
        // c68 holds the same camera: its exact bits (the derived one carries the
        // object's float rounding).
        if (Finite(c68) && camera::Close(c68, pv))
        {
          pv = c68;
          out.source = kSourceC68;
        }
        else
        {
          out.source = kSourceObject;
          ++c68_stale;
        }
      }
      else if (block(vs.object_to_clip) && !vs.light_shell)
      {
        // Only the object's clip matrix: the camera is c68 if this frame loaded it.
        *reason = "stale-c68";
        if (c68_frame != frame || !Finite(c68))
          return false;
        pv = c68;
        out.source = kSourceC68;
      }
      else
        return false;
      *reason = "projection";
      if (!Finite(pv) || !camera::Split(pv, out.camera))
        return false;
      return true;
    }

    bool Queue(uint32_t device_address, ScenePoint point, const DeviceSnapshot &state, uint64_t source_hash = 0,
               const ViewCamera *view = nullptr, uint32_t reset = kResetNone)
    {
      using namespace rex::graphics::title;
      auto *device = Access(device_address, kDeviceBytes, true);
      if (!device)
      {
        ++invalid;
        return false;
      }
      SceneMarker marker;
      marker.point = point;
      marker.frame = frame;
      marker.session = session;
      marker.device = device_address;
      marker.source_hash = source_hash;
      if (state.viewport_valid)
      {
        marker.width = state.viewport[2];
        marker.height = state.viewport[3];
      }
      else
      {
        marker.width = view_width ? view_width : 1280;
        marker.height = view_height ? view_height : 720;
      }
      if (Series())
        marker.flags |= kSceneCapture;
      if (point == ScenePoint::kView)
      {
        if (!view || !state.viewport_valid)
          return false;
        marker.camera.view = view->camera.view;
        marker.camera.projection = view->camera.projection;
        marker.camera.view_projection = view->camera.view_projection;
        marker.flags |= kSceneCamera;
        marker.camera_register = view->camera_register;
        marker.delta_time = frame_seconds;
        marker.meters_per_unit = float(REXCVAR_GET(fuse_engine_scene_meters_per_unit));
        if (REXCVAR_GET(fuse_engine_scene_auto_opaque))
          marker.flags |= kSceneAutoOpaque;
        marker.reset_reason = reset;
        if (reset)
          marker.flags |= kSceneReset;
      }
      const uint32_t cursor = Be32(device + kRingCursor), safe_end = Be32(device + kRingSafeEnd);
      constexpr uint32_t bytes = (uint32_t(kSceneWords) + 1) * 4;
      // Append only inside already reserved space. Never relocate the segment,
      // call a reserve helper or run title work twice.
      if ((cursor & 3) || uint64_t(cursor) + bytes > safe_end || uint64_t(cursor) + bytes + 4 > 0x100000000ull)
      {
        ++ring_skips;
        ++skips_by_point[uint32_t(point) & 7];
        return false;
      }
      auto *output = Access(cursor + 4, bytes, true);
      if (!output)
      {
        ++invalid;
        return false;
      }
      const auto words = EncodeScene(marker);
      PutBe32(output, SceneHeader());
      for (size_t i = 0; i < words.size(); ++i)
        PutBe32(output + 4 + i * 4, words[i]);
      PutBe32(device + kRingCursor, cursor + bytes);
      ++queued;
      if (Tracing())
        trace += fmt::format("{{\"f\":{},\"e\":\"marker\",\"point\":{},\"w\":{},\"h\":{}}}\n", frame, uint32_t(point),
                             marker.width, marker.height);
      return true;
    }
    bool Snapshot(uint32_t device, DeviceSnapshot &state)
    {
      const auto *bytes = Access(device, kDeviceBytes);
      if (!bytes || !SnapshotDevice({bytes, kDeviceBytes}, state))
        return false;
      // The bound shaders: the device's slots miss GpuLoadShaders.
      if (bound_seen.load(std::memory_order_relaxed))
      {
        state.vertex_shader = bound_vertex_shader.load(std::memory_order_relaxed);
        state.pixel_shader = bound_pixel_shader.load(std::memory_order_relaxed);
      }
      return true;
    }
    bool StreamWanted()
    {
      if (REXCVAR_GET(fuse_engine_scene_stream))
        return true;
      // The GPU plugin's flags, by name (they live in its module).
      const auto frames = rex::cvar::GetFlagByName("native_scene_frames");
      const auto upscaler = rex::cvar::GetFlagByName("native_temporal_upscaler");
      const auto generation = rex::cvar::GetFlagByName("native_frame_generation");  // The presenter's.
      return frames == "true" || frames == "1" || (!upscaler.empty() && upscaler != "off") ||
             (!generation.empty() && generation != "off");
    }
    void UpdateActive() { active.store(Producing() || Tracing(), std::memory_order_release); }
    // Split-screen: camera draws at side-by-side viewports.
    void NoteCameraViewport(const DeviceSnapshot &state)
    {
      for (const auto &v : camera_viewports)
        if (v == state.viewport)
          return;
      if (camera_viewports.size() < 8)
        camera_viewports.push_back(state.viewport);
    }
    bool SplitViewports()
    {
      for (size_t i = 0; i < camera_viewports.size(); ++i)
        for (size_t j = i + 1; j < camera_viewports.size(); ++j)
        {
          const auto &a = camera_viewports[i], &b = camera_viewports[j];
          const bool apart = a[0] + a[2] <= b[0] || b[0] + b[2] <= a[0] || a[1] + a[3] <= b[1] || b[1] + b[3] <= a[1];
          if (apart)
            return true;
        }
      return false;
    }
  } // namespace

  void Initialize(rex::Runtime &runtime, const std::filesystem::path &report_directory)
  {
    std::lock_guard lock(mutex);
    graphics = runtime.graphics_system();
    // The backend takes this protocol or a newer one.
    if (!graphics || graphics->GetTitleSceneProtocolVersion() < rex::graphics::title::kSceneProtocolVersion)
    {
      if (StreamWanted())
        REXLOG_WARN("Fuse scene: GPU backend does not support scene protocol {} (native plugin on Vulkan only)",
                    rex::graphics::title::kSceneProtocolVersion);
      return;
    }
    memory = runtime.memory();
    stream = StreamWanted();
    session = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
    directory = report_directory;
    if (!directory.empty())
    {
      report_path = directory / "scene-producer.json";
      trace_path = directory / "trace.jsonl";
      if (!rex::cvar::SetFlagByName("native_scene_probe_dir", (directory / "native").generic_string()) ||
          !rex::cvar::SetFlagByName("native_scene_probe_tracking", "true"))
      {
        REXLOG_ERROR("Fuse scene: output directory could not be configured");
        return;
      }
    }
    rex::cvar::RegisterChangeCallback("fuse_engine_capture_request", [](std::string_view, std::string_view value)
                                      {
      if (value != "0") capture_requested.store(true, std::memory_order_release); });
    if (REXCVAR_GET(fuse_engine_capture_request))
      capture_requested.store(true, std::memory_order_release);
    // The stream follows the consumer while the game runs (F4), from the next frame.
    for (const char *name : {"native_temporal_upscaler", "native_scene_frames", "fuse_engine_scene_stream"})
      rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view)
                                        { stream_changed.store(true, std::memory_order_release); });
    last_swap = std::chrono::steady_clock::now();
    enabled.store(true, std::memory_order_release);
    UpdateActive();
    Report();
    REXLOG_INFO("Fuse scene: protocol {} enabled, stream {}, session {}, reports {}",
                rex::graphics::title::kSceneProtocolVersion, stream, session,
                directory.empty() ? std::string("off") : directory.string());
  }

  bool Enabled() { return enabled.load(std::memory_order_relaxed); }
  bool Active() { return active.load(std::memory_order_relaxed); }

  namespace
  {
    // A shader's entry from its container header (the creation's, or the
    // object's copy of it). False: not a container of that kind.
    bool Register(bool pixel, uint32_t object, const uint8_t *bytes, uint32_t header_bytes, uint64_t hash,
                  uint32_t address)
    {
      ShaderContainer parsed;
      if (!ParseShaderContainer(bytes, header_bytes, parsed) || parsed.pixel != pixel)
        return false;
      Shader shader;
      shader.name = parsed.name.empty() ? fmt::format("{}_{:08X}", pixel ? "PS" : "VS", address) : parsed.name;
      shader.pixel = pixel;
      shader.hash = hash;
      shader.world_to_clip = parsed.Float("g_VP_WorldToClipMat");
      auto ends = [](std::string_view text, std::string_view suffix)
      { return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix; };
      for (const auto &c : parsed.constants)
      {
        if (c.register_set != 2 || c.count < 4)
          continue;
        if (ends(c.name, "ObjToClipMat") && shader.object_to_clip < 0)
          shader.object_to_clip = c.index;
        if (ends(c.name, "ObjToWorldMat") && shader.object_to_world < 0)
          shader.object_to_world = c.index;
      }
      shader.light_shell = shader.name.find("LightShell") != std::string::npos;
    shader.hud = (!pixel && parsed.Float("g_Scaleform_TransformMat") >= 0) ||
                   shader.name.find("Scaleform") != std::string::npos;
      for (const auto prefix : kPostShaders)
        shader.post |= pixel && std::string_view(shader.name).substr(0, prefix.size()) == prefix;
      // The engine caches its shaders: a known object keeps its entry.
      if (const auto known = shader_by_object.find(object);
          known != shader_by_object.end() && shaders[known->second].hash == shader.hash)
        return true;
      shader_by_object[object] = uint32_t(shaders.size());
      shaders.push_back(std::move(shader));
      return true;
    }
    // The bound shader's entry; one not seen created is identified from its
    // object, which holds a copy of its container header (XGSetShaderHeader:
    // vertex shaders at + 872, pixel shaders at + 40).
    const Shader *Identify(uint32_t object, bool pixel)
    {
      if (!object)
        return nullptr;
      if (const auto found = shader_by_object.find(object); found != shader_by_object.end())
        return shaders[found->second].pixel == pixel ? &shaders[found->second] : nullptr;
      const uint32_t header = object + (pixel ? 40 : 872);
      bool known = false;
      if (const auto *head = Access(header, 32))
        if (const uint32_t bytes = Be32(head + 4); bytes >= 32 && bytes <= 65536)
          if (const auto *copy = Access(header, bytes))
            known = Register(pixel, object, copy, bytes, XXH3_64bits(copy, bytes), object);
      if (!known)
      {
        // Not a container: remembered, so it is not parsed again.
        Shader unknown;
        unknown.name = fmt::format("{}_{:08X}_unknown", pixel ? "PS" : "VS", object);
        unknown.pixel = pixel;
        shader_by_object[object] = uint32_t(shaders.size());
        shaders.push_back(std::move(unknown));
      }
      return &shaders[shader_by_object[object]];
    }
  } // namespace

  void OnBindShaders(uint32_t vertex_shader, uint32_t pixel_shader, bool vertex, bool pixel)
  {
    if (vertex)
      bound_vertex_shader.store(vertex_shader, std::memory_order_relaxed);
    if (pixel)
      bound_pixel_shader.store(pixel_shader, std::memory_order_relaxed);
    bound_seen.store(true, std::memory_order_relaxed);
  }

  void OnCreateShader(bool pixel, uint32_t container, uint32_t object)
  {
    if (!enabled.load(std::memory_order_acquire) || !object)
      return;
    std::lock_guard lock(mutex);
    const auto *head = Access(container, 32);
    if (!head)
      return;
    const uint32_t header_bytes = Be32(head + 4), code_bytes = Be32(head + 8);
    if (header_bytes < 32 || header_bytes > 65536 || !code_bytes || code_bytes > 1024 * 1024)
      return;
    if (const auto *bytes = Access(container, header_bytes + code_bytes))
      Register(pixel, object, bytes, header_bytes, XXH3_64bits(bytes + header_bytes, code_bytes), container);
  }

  void BeforeDraw(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    if (!Producing() && !Tracing())
      return;
    Scan(device);  // The draw's constants, written before it.
    if (!Producing())
      return;
    DeviceSnapshot state;
    if (!Snapshot(device, state))
      return;
    // (Identify may add entries: one lookup at a time.)
    const Shader *ps = Identify(state.pixel_shader, true);
    if (ps && ps->post && post_frame != frame)
      post_frame = frame;
    const Shader *vs = Identify(state.vertex_shader, false);
    // The HUD follows the post chain: the frame without it is bound now.
    if (vs && vs->hud && scene_end_frame == frame && post_end_frame != frame && !state.tiling &&
        state.viewport_valid && state.Area() * 10 >= uint64_t(view_width) * view_height * 9 &&
        Queue(device, ScenePoint::kPostEnd, state))
    {
      post_end_frame = frame;
      ++post_end_hud;
    }
  }

  void AfterDraw(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    const bool tracing = Tracing();
    if (!Producing() && !tracing)
      return;
    Scan(device);
    DeviceSnapshot state;
    if (!Snapshot(device, state))
      return;
    if (tracing)
      Identify(state.pixel_shader, true);  // Named in the trace (before vs: it may add entries).
    const Shader *vs = Identify(state.vertex_shader, false);
    const bool camera_shader = vs && !vs->pixel && vs->Camera();
    std::string camera_note;
    if (camera_shader && !state.tiling && state.viewport_valid && state.Area() >= kMinViewArea)
      NoteCameraViewport(state);
    // kView: the 1st, 8th and 32nd camera draw of the frame at a large viewport.
    // (Before the scene end: a view after it would begin the frame anew.)
    const bool wanted = Producing() && !split_screen && camera_draws < 32 && scene_end_frame != frame;
    if (camera_shader && (wanted || tracing))
    {
      const char *reason = nullptr;
      ViewCamera view;
      if (state.tiling)
        reason = "tiling";
      else if (!state.viewport_valid || state.Area() < kMinViewArea)
        reason = "viewport";
      else if (REXCVAR_GET(fuse_engine_scene_view_hdr) && !DeviceSnapshot::HdrColor(state.color_info))
        reason = "target";
      else if (DrawCamera(*vs, view, &reason))
        reason = nullptr;
      if (tracing && traced_camera_draws < 3)
      {
        // Raw c68-c87 (camera, camera position, object matrices) for diagnosis.
        ++traced_camera_draws;
        camera_note += ",\"c68\":[";
        for (uint32_t i = 0; i < 80; ++i)
        {
          const float value = std::bit_cast<float>(alu[kWorldToClipRegister * 4 + i]);
          camera_note += (i ? "," : "") + (std::isfinite(value) ? fmt::format("{}", value) : std::string("null"));
        }
        camera_note += "]";
      }
      if (tracing)
        camera_note += reason ? fmt::format(",\"camera\":\"{}\"", reason)
                             : fmt::format(",\"camera\":\"{}\",\"eye\":[{:.4f},{:.4f},{:.4f}],\"campos\":[{:.4f},{:.4f},{:.4f}]",
                                           kSourceNames[view.source], view.camera.eye[0], view.camera.eye[1],
                                           view.camera.eye[2], view.position[0], view.position[1], view.position[2]);
      if (wanted)
      {
        if (reason)
        {
          frame_refusal = reason;
          const std::string_view why(reason);
          if (marked_view == frame)
          {
            // Counted for frames still without a view only.
          }
          else if (why == "tiling")
            ++view_refused_tiling;
          else if (why == "viewport")
          {
            ++view_refused_viewport;
            const uint32_t size = state.viewport[2] << 16 | state.viewport[3];
            if (refused_viewports.size() < 32 || refused_viewports.contains(size))
              ++refused_viewports[size];
          }
          else if (why == "projection")
            ++view_refused_projection;
          else if (why == "target")
            ++view_refused_target;
          else
            ++view_refused_matrix;
        }
        else if (const uint32_t index = ++camera_draws;
                 index == 1 || index == 8 || index == 32 || marked_view != frame)
        {
          double jump = 0, offset = 0;
          for (size_t i = 0; i < 3; ++i)
          {
            jump += (view.camera.eye[i] - previous_eye[i]) * (view.camera.eye[i] - previous_eye[i]);
            offset += (view.camera.eye[i] - view.position[i]) * (view.camera.eye[i] - view.position[i]);
          }
          const double cut = REXCVAR_GET(fuse_engine_scene_cut_distance);
          const uint32_t reset =
              previous_eye_frame && previous_eye_frame + 1 == frame && jump > cut * cut ? kResetTeleport : kResetNone;
          if (offset > .05 * .05)
            ++eye_mismatch;  // Diagnostic: g_VP_WSCamPos is another camera's (or stale).
          if (Queue(device, ScenePoint::kView, state, vs->hash, &view, reset))
          {
            if (marked_view == frame)
              ++view_retries;
            else
            {
              ++views;
              ++views_by_source[view.source];
              if (view_shaders.size() < 32 || view_shaders.contains(vs->name))
                ++view_shaders[vs->name];
              if (reset)
                ++resets_teleport;
            }
            marked_view = frame;
            view_state = state;
            view_width = state.viewport[2];
            view_height = state.viewport[3];
            frame_eye = view.camera.eye;
            if (tracing)
              trace += fmt::format("{{\"f\":{},\"e\":\"view\",\"source\":\"{}\",\"register\":{},\"view\":[{}],"
                                   "\"projection\":[{}],\"pv\":[{}]}}\n",
                                   frame, kSourceNames[view.source], int32_t(view.camera_register),
                                   fmt::join(view.camera.view, ","), fmt::join(view.camera.projection, ","),
                                   fmt::join(view.camera.view_projection, ","));
          }
        }
      }
    }
    if (tracing)
    {
      TraceShaders();
      trace += fmt::format("{{\"f\":{},\"e\":\"draw\",\"i\":{},{}{}}}\n", frame, draw_ordinal, TraceState(state),
                           camera_note);
    }
    ++draw_ordinal;
  }

  void BeforeResolve(uint32_t device, uint32_t site, uint32_t flags)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    const bool tracing = Tracing();
    if (!Producing() && !tracing)
      return;
    Scan(device);
    const uint32_t call = std::min<uint32_t>(++resolve_calls[site], 255);
    const uint64_t key = uint64_t(site) << 8 | call;
    DeviceSnapshot state;
    if (!Snapshot(device, state))
      return;
    // Colour of render target 0 (flags: target index, 4 = depth).
    const bool color = (flags & 7) == 0;
    // The view's colour target, its depth still bound, before the post chain.
    // The EDRAM base only: for a 16-bit float texture the engine resolves
    // through an alias of the target in another format (sub_82881D70).
    const bool candidate = color && marked_view == frame && post_frame != frame && state.viewport_valid &&
                           state.viewport[2] == view_width && state.viewport[3] == view_height &&
                           (state.color_info & 0xFFF) == (view_state.color_info & 0xFFF) &&
                           state.depth_info == view_state.depth_info;
    if (candidate)
      frame_candidate = key;
    if (tracing)
      trace += fmt::format("{{\"f\":{},\"e\":\"resolve\",\"site\":\"{:08X}\",\"call\":{},\"flags\":\"{:X}\","
                           "\"candidate\":{},\"post\":{},{}}}\n",
                           frame, site, call, flags, candidate, post_frame == frame, TraceState(state));
    // Capture series: a target snapshot before every scene-sized colour resolve
    // (the call site in the probe's source_hash).
    if (Series() && REXCVAR_GET(fuse_engine_scene_probe) && color && state.viewport_valid &&
        state.Area() >= kMinViewArea)
      Queue(device, ScenePoint::kProbe, state, key);
    if (!Producing() || marked_view != frame)
      return;
    if (scene_end_frame == frame)
    {
      // The frame after its post chain: a scene-sized colour resolve.
      if (!color || post_frame != frame || state.tiling || !state.viewport_valid ||
          state.Area() * 10 < uint64_t(view_width) * view_height * 9)
        return;
      frame_last_resolve = key;
      if (post_end_frame != frame && frame_ends.Has(key) && Queue(device, ScenePoint::kPostEnd, state))
      {
        post_end_frame = frame;
        ++post_end_resolve;
      }
      return;
    }
    const uint64_t configured = ConfiguredEnd();
    if (configured ? key == configured : candidate && scene_ends.Has(key))
    {
      const uint64_t skips = ring_skips;
      if (Queue(device, ScenePoint::kSceneEnd, state))
      {
        scene_end_frame = frame;
        ++scene_end_at_resolve;
      }
      else if (ring_skips != skips && !(flags & ~7u))  // Not a clearing resolve: the target stays.
      {
        scene_end_retry = frame;
        scene_end_retry_state = state;
      }
    }
  }

  void AfterResolve(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    if (scene_end_retry != frame || scene_end_frame == frame || !Producing())
      return;
    scene_end_retry = 0;
    if (Queue(device, ScenePoint::kSceneEnd, scene_end_retry_state))
    {
      scene_end_frame = frame;
      ++scene_end_at_resolve;
      ++scene_end_after_resolve;
    }
  }

  void BeforeKick(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    Scan(device);  // The segment's end.
  }

  void AfterKick(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    if (const auto *bytes = Access(device + kRingCursor, 4))
      scan_cursor = Be32(bytes);  // The new segment, nothing in it yet.
  }

  void BeforeSwap(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    if (!Producing())
      return;
    Scan(device);
    DeviceSnapshot state;
    if (!Snapshot(device, state))
      return;
    // No HUD this frame (cutscenes, menus without Scaleform): the frame as it is.
    if (scene_end_frame == frame && post_end_frame != frame)
    {
      DeviceSnapshot frame_state = state;
      if (!frame_state.viewport_valid && view_width)
      {
        frame_state.viewport = {0, 0, view_width, view_height};
        frame_state.viewport_valid = true;
      }
      if (Queue(device, ScenePoint::kPostEnd, frame_state))
      {
        post_end_frame = frame;
        ++post_end_swap;
      }
    }
    const uint64_t skips = ring_skips;
    if (!Queue(device, ScenePoint::kFrameEnd, state) && ring_skips != skips)
    {
      frame_end_retry = true;
      frame_end_retry_state = state;
    }
  }

  void OnSwap(uint32_t device)
  {
    if (!enabled.load(std::memory_order_acquire))
      return;
    std::lock_guard lock(mutex);
    // The frame end the full segment refused: the swap has submitted it. It
    // arms the next frame's jitter and object motion (without it the frame
    // after has no object motion: characters with the camera's only).
    if (frame_end_retry)
    {
      frame_end_retry = false;
      if (Producing() && Queue(device, ScenePoint::kFrameEnd, frame_end_retry_state))
        ++frame_end_after_swap;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_swap).count();
    last_swap = now;
    frame_seconds = seconds > 0 && seconds < .25 ? float(seconds) : 0.f;
    if (Producing())
    {
      if (marked_view != frame && !split_screen)
      {
        ++frames_without_view;
        const std::string why = frame_refusal ? frame_refusal : "no-camera-draw";
        if (no_view_reasons.size() < 16 || no_view_reasons.contains(why))
          ++no_view_reasons[why];
      }
      if (marked_view == frame)
      {
        previous_eye = frame_eye;
        previous_eye_frame = frame;
      }
      if (marked_view == frame && scene_end_frame != frame)
        ++scene_end_missed;
      if (marked_view == frame && frame_candidate && scene_ends.Note(frame_candidate))
        REXLOG_INFO("Fuse scene: scene end at the resolve called from {} (call {} of the frame); learned: {}",
                    Hex(frame_candidate >> 8), frame_candidate & 255, scene_ends.Text());
      if (scene_end_frame == frame && frame_last_resolve && frame_ends.Note(frame_last_resolve))
        REXLOG_INFO("Fuse scene: frame end at the resolve called from {} (call {} of the frame); learned: {}",
                    Hex(frame_last_resolve >> 8), frame_last_resolve & 255, frame_ends.Text());
      const bool split = SplitViewports();
      if (split != split_screen)
        REXLOG_INFO("Fuse scene: split-screen {} (one view per frame only: original image)", split ? "on" : "off");
      split_screen = split;
      if (split)
        ++split_frames;
    }
    if (Tracing())
    {
      trace += fmt::format("{{\"f\":{},\"e\":\"swap\",\"draws\":{},\"seconds\":{:.5f}}}\n", frame, draw_ordinal,
                           seconds);
      FlushTrace();
    }
    resolve_calls.clear();
    camera_viewports.clear();
    frame_candidate = frame_last_resolve = 0;
    camera_draws = 0;
    frame_refusal = nullptr;
    draw_ordinal = 0;
    traced_camera_draws = 0;
    ++frame;
    if (stream_changed.exchange(false, std::memory_order_acq_rel) && stream != StreamWanted())
    {
      // A new stream starts with a history reset (the SDK sees a frame gap).
      stream = !stream;
      REXLOG_INFO("Fuse scene: stream {}", stream ? "on" : "off");
    }
    const bool finished = first && frame == end;
    // A capture series (kSceneCapture frames) starts after the current one ends.
    if (frame >= end && capture_requested.exchange(false, std::memory_order_acq_rel))
    {
      first = frame + uint64_t(REXCVAR_GET(fuse_engine_capture_delay_frames));
      end = first + uint64_t(REXCVAR_GET(fuse_engine_capture_frames));
      trace_first = first;
      trace_end = end;
      REXLOG_INFO("Fuse scene: capture of {} frames from {}", end - first, first);
    }
    // Periodic traces (diagnostics).
    if (const int interval = REXCVAR_GET(fuse_engine_trace_interval);
        interval > 0 && !trace_path.empty() && frame >= trace_end && frame % uint64_t(interval) == 0)
    {
      trace_first = frame;
      trace_end = frame + uint64_t(REXCVAR_GET(fuse_engine_trace_frames));
    }
    UpdateActive();
    if (!Producing() && !Tracing())
      scan_cursor = 0;  // Not read meanwhile: the next frame starts over.
    if (finished || frame % 600 == 0)
      Report();
  }

  void Shutdown()
  {
    if (!enabled.exchange(false, std::memory_order_acq_rel))
      return;
    std::lock_guard lock(mutex);
    FlushTrace();
    Report();
    active.store(false, std::memory_order_release);
    memory = nullptr;
    graphics = nullptr;
  }
} // namespace fuse::engine::scene
