#include <memory>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "../game_cvars.h"
#include "fov.h"

namespace
{
  class FovDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit FovDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO & /*io*/) override
    {
      ImGui::SetNextWindowSize(ImVec2(480, 200), ImGuiCond_FirstUseEver);
      if (!ImGui::Begin("Field of view##fuse", nullptr, ImGuiWindowFlags_NoCollapse))
      {
        ImGui::End();
        return;
      }

      float percent = static_cast<float>(REXCVAR_GET(fuse_fov_scale) * 100.0);
      if (ImGui::SliderFloat("Field of view", &percent, float(fuse::fov::kScaleMin * 100.0),
                             float(fuse::fov::kScaleMax * 100.0), "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
      {
        REXCVAR_SET(fuse_fov_scale, static_cast<double>(percent) / 100.0);
      }
      if (ImGui::Button("Reset to original (100%)"))
      {
        REXCVAR_SET(fuse_fov_scale, 1.0);
      }
      const float game = fuse::fov::LastGameFov();
      if (game > 1.f)
        ImGui::Text("Camera: %.1f -> %.1f degrees (horizontal)", game, fuse::fov::LastAppliedFov());
      ImGui::TextWrapped("Updates on the next gameplay camera frame. To keep this value, "
                         "use Save to config in the F4 Settings overlay.");
      ImGui::End();
    }
  };

  std::unique_ptr<FovDialog> g_dialog;
} // namespace

namespace fuse::fov
{
  void Init(rex::ui::ImGuiDrawer *drawer)
  {
    rex::ui::RegisterBind("bind_fov_settings", "F6", "Toggle field of view slider", [drawer]
                          {
      if (g_dialog)
      {
        g_dialog.reset();
      }
      else
      {
        g_dialog = std::make_unique<FovDialog>(drawer);
      } });
  }

  bool IsOpen()
  {
    return g_dialog != nullptr;
  }

  void Shutdown()
  {
    rex::ui::UnregisterBind("bind_fov_settings");
    g_dialog.reset();
  }
} // namespace fuse::fov
