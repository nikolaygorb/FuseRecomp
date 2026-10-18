// fuserecomp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

#include "debug_tools.h"
#include "game_cvars.h"
#include "game_patches.h"
#include "utils.h"

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#endif

class FuserecompApp : public rex::ReXApp
{
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext &ctx)
  {
    return std::unique_ptr<FuserecompApp>(new FuserecompApp(ctx, "fuserecomp",
                                                          PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig &config) override
  {
    std::string backend = REXCVAR_GET(graphics_backend);
    if (backend != "any" && !config.gpu_plugin.empty())
    {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics)
      {
        REXLOG_WARN("graphics_backend '{}' unavailable, falling back to automatic selection",
                    backend);
      }
    }
  }

  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostLoadXexImage() override
  {
    game_patches::ApplyEnabledPatches();
  }

  void OnPostSetup() override
  {
    utils::LoadSettingsFiles();

    MountUtilityCache();

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    // SDK exposes the perf_log_csv cvar but never wires it up; do it here so
    // --perf_log_csv=<path> actually produces per-frame CSV output.
    // Use GetFlagByName to avoid needing a cross-DLL declaration of the cvar.
    rex::perf::SetCsvLogPath(rex::cvar::GetFlagByName("perf_log_csv"));
#endif

    // Run debug tools only if enabled via hardware.toml (dev_debug_runtime = true)
    if (REXCVAR_GET(dev_debug_runtime))
    {
      debug_tools::PerformMissingFunctionScan();
      debug_tools::PerformStubSweep();
    }
  }

  // Fuse mounts the console's utility-drive cache through XMountUtilityDrive,
  // which leaves cache: => \Device\cache1, and streams through
  // cache:\stream.arc.cache.* (~800 MB). The SDK mounts no device there, so
  // every cache: open fails. Back it with a host folder, like xenia-canary's
  // mount_cache. Runs before LaunchModule, so it is in place before the title
  // creates the link.
  void MountUtilityCache()
  {
    if (!REXCVAR_GET(fuse_mount_cache))
    {
      return;
    }

    auto *fs = runtime() ? runtime()->file_system() : nullptr;
    if (!fs)
    {
      return;
    }

    constexpr const char *kMountPath = "\\Device\\cache1";
    auto host_path = cache_root() / "cache1";
    auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, host_path,
                                                                   false, true);
    if (!device->Initialize() || !fs->RegisterDevice(std::move(device)))
    {
      REXLOG_WARN("Cannot mount utility cache {} at {}", kMountPath, host_path.string());
      return;
    }
    REXLOG_INFO("Mounted utility cache {} at {}", kMountPath, host_path.string());
  }

  void OnShutdown() override
  {
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    rex::perf::FlushCsv();
#endif
  }

  void OnConfigurePaths(rex::PathConfig &paths) override
  {
    // --game_data_root / REX_GAME_DATA_ROOT still win: this cvar is read
    // before any config file loads, so it's already non-empty here if set.
    if (paths.game_data_root.empty())
    {
      paths.game_data_root = utils::RepoRoot() / "assets";
    }

    // Hardware/rendering settings become the SDK's primary config file, so
    // the in-game Settings overlay's "Save to config" also lands here.
    paths.config_path = utils::SettingsDir() / "hardware.toml";

    // Input mapping lives in its own file, loaded manually below (the SDK
    // only auto-loads the single path above).
    utils::LoadSettingsFiles();
  }

public:
  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
