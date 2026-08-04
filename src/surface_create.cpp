/**
 * ANativeWindowCreator::Create — ANWC header only declares it.
 * Copied from SkiaDraw secure_capture (without skia_draw dependency).
 */
#include "a_native_window_creator.h"

namespace android {

ANativeWindow* ANativeWindowCreator::Create(const CreateOptions& options) {
  auto& surfaceComposerClient = GetComposerInstance();

  int32_t width = options.width;
  int32_t height = options.height;
  while (width == 0 || height == 0) {
    anative_window_creator::detail::types::ui::DisplayState displayInfo{};
    if (!surfaceComposerClient.GetDisplayInfo(&displayInfo))
      break;
    width = displayInfo.layerStackSpaceRect.width;
    height = displayInfo.layerStackSpaceRect.height;
    break;
  }

  auto surfaceControl = surfaceComposerClient.CreateSurface(
      options.name, width, height, {}, options.skipScreenshot);
  auto* nativeWindow =
      reinterpret_cast<ANativeWindow*>(surfaceControl.GetSurface());
  m_cachedSurfaceControl.emplace(nativeWindow, std::move(surfaceControl));
  return nativeWindow;
}

}  // namespace android
