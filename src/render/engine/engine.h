#pragma once

namespace fuse::engine
{
  // Before the GPU starts (the native plugin loaded): Fuse's defaults for the
  // SDK's scene consumer, where nothing set them.
  void ConfigureRenderer();
  void Initialize();
  void Shutdown();
} // namespace fuse::engine
