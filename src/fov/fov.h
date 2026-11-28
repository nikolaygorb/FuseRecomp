#pragma once

namespace rex::ui
{
  class ImGuiDrawer;
} // namespace rex::ui

namespace fuse::fov
{
  constexpr double kScaleMin = 0.75;
  constexpr double kScaleMax = 2.2;

  // Horizontal FOV in radians with fuse_fov_scale applied: the view's width and height grow by the scale.
  double Apply(double game_fov);

  // The scene camera's last horizontal FOV in degrees, before / after the scale (0 until the first frame).
  float LastGameFov();
  float LastAppliedFov();

  // Registers the dialog hotkey (bind_fov_settings, default F6).
  void Init(rex::ui::ImGuiDrawer *drawer);

  bool IsOpen();

  // Closes the dialog. Must run while the ImGui drawer is still alive.
  void Shutdown();
} // namespace fuse::fov
