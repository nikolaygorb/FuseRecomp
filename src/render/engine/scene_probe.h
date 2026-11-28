#pragma once
#include <cstdint>
#include <filesystem>
namespace rex
{
  class Runtime;
}
// Fuse scene producer: annotates the title's command stream with SDK scene
// markers (rex/graphics/title_scene.h). All Fuse knowledge of the camera, the
// shaders and the frame's passes lives here.
namespace fuse::engine::scene
{
  // Starts tracking when the GPU plugin takes scene markers. The stream runs
  // while the SDK's consumer is on (a temporal upscaler, frame generation,
  // native_scene_frames, fuse_engine_scene_stream), switched at the next
  // frame, or for a capture. Empty directory: no diagnostic files.
  void Initialize(rex::Runtime &runtime, const std::filesystem::path &directory);
  bool Enabled();  // Tracking (shader creation, swaps).
  bool Active();   // Markers or a trace this frame.
  void OnCreateShader(bool pixel, uint32_t container, uint32_t object);
  // The title binds its shaders with GpuLoadShaders (which leaves the
  // device's shader slots alone) or SetVertexShader / SetPixelShader: the
  // last binding of each holds. Cheap, from the start.
  void OnBindShaders(uint32_t vertex_shader, uint32_t pixel_shader, bool vertex, bool pixel);
  void BeforeDraw(uint32_t device);
  void AfterDraw(uint32_t device);
  // site: where the title asked for the resolve (its engine wrapper's caller).
  void BeforeResolve(uint32_t device, uint32_t site, uint32_t flags);
  void AfterResolve(uint32_t device);  // A scene end the full ring segment refused.
  // D3D's ring segment change (the producer reads the title's packets).
  void BeforeKick(uint32_t device);
  void AfterKick(uint32_t device);
  void BeforeSwap(uint32_t device);
  void OnSwap(uint32_t device);  // After the guest Swap: the next frame.
  void Shutdown();
} // namespace fuse::engine::scene
