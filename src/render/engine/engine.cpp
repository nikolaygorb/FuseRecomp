#include "engine.h"

#include "scene_probe.h"

#include <chrono>
#include <filesystem>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(fuse_engine_report_dir, "", "Render/Engine",
                      "Scene diagnostics parent folder (producer report, trace, SDK probe and frame exports); "
                      "each run creates a subfolder. Empty: no files.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace fuse::engine
{
  void ConfigureRenderer()
  {
    // Object motion tells copies of a mesh apart by the object's world
    // transform: *ObjToWorldMat c80-c83 in all of Fuse's model VS. Its default
    // (the first used constants) includes the object-to-clip matrix, which a
    // camera turn changes more than the distance between copies, so copies
    // swapped histories (wrong motion on static geometry). "off": the SDK's.
    constexpr const char *kIdentity = "native_scene_object_motion_identity";
    if (rex::cvar::GetFlagByName(kIdentity).empty() && rex::cvar::SetFlagByName(kIdentity, "80-83"))
      REXLOG_INFO("Fuse engine: {} = 80-83 (ObjToWorld)", kIdentity);
  }

  // The XEX is the one the code was generated from (the SDK's xex_image_check),
  // so the hooked functions and Fuse's addresses hold.
  void Initialize()
  {
    auto *runtime = rex::Runtime::instance();
    if (!runtime || !runtime->memory())
      return;
    std::filesystem::path directory;
    if (const auto requested = REXCVAR_GET(fuse_engine_report_dir); !requested.empty())
    {
      directory = std::filesystem::path(requested) /
                  fmt::format("scene-{}", std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
      std::error_code error;
      if (!std::filesystem::create_directories(directory, error) || error)
      {
        REXLOG_ERROR("Fuse engine: cannot create report directory {}", directory.string());
        directory.clear();
      }
    }
    scene::Initialize(*runtime, directory);
  }

  void Shutdown() { scene::Shutdown(); }
} // namespace fuse::engine
