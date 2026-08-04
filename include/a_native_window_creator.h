// MINER_PATCHED_NO_EXCEPTIONS
/*
 * MIT License
 *
 * Copyright (c) 2023 Bzi-Han
 * Project: https://github.com/Bzi-Han/AndroidSurfaceImgui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

 #ifndef A_NATIVE_WINDOW_CREATOR_H  // !A_NATIVE_WINDOW_CREATOR_H
 #define A_NATIVE_WINDOW_CREATOR_H
 
 #include <android/log.h>
 #include <android/native_window.h>
 #include <dlfcn.h>
 #include <sys/system_properties.h>
 
 #include <algorithm>
 #include <array>
 #include <charconv>
 #include <cstdint>
 #include <cstdio>
 #include <cstdlib>
 #include <memory>
 #include <string>
 #include <string_view>
 #include <unordered_map>
 #include <unordered_set>
 #include <vector>
 
 #ifndef LOGTAG
 #define LOGTAG "AImGui"
 
 #define LOGTAG_DEFINED 1
 #endif  // !TAG
 
 #ifndef LogInfo
// silence logs
#define LogInfo(formatter, ...) ((void)0)
#define LogDebug(formatter, ...) ((void)0)
#define LogError(formatter, ...) ((void)0)
#define LogInfo_DEFINED 1
#endif  // !LogInfo
 
 namespace android::anative_window_creator::detail::types {
 /**
  * The following types and structures are adapted from AOSP.
  */
 
 enum class PixelFormat {
   UNKNOWN = 0,
   CUSTOM = -4,
   TRANSLUCENT = -3,
   TRANSPARENT = -2,
   OPAQUE = -1,
   RGBA_8888 = 1,
   RGBX_8888 = 2,
   RGB_888 = 3,
   RGB_565 = 4,
   BGRA_8888 = 5,
   RGBA_5551 = 6,
   RGBA_4444 = 7,
   RGBA_FP16 = 22,
   RGBA_1010102 = 43,
   R_8 = 0x38,
 };
 
 enum class WindowFlags : uint32_t {  // (keep in sync with SurfaceControl.java)
   eHidden = 0x00000004,
   eDestroyBackbuffer = 0x00000020,
   eSkipScreenshot = 0x00000040,
   eSecure = 0x00000080,
   eNonPremultiplied = 0x00000100,
   eOpaque = 0x00000400,
   eProtectedByApp = 0x00000800,
   eProtectedByDRM = 0x00001000,
   eCursorWindow = 0x00002000,
   eNoColorFill = 0x00004000,
 
   eFXSurfaceBufferQueue = 0x00000000,
   eFXSurfaceEffect = 0x00020000,
   eFXSurfaceBufferState = 0x00040000,
   eFXSurfaceContainer = 0x00080000,
   eFXSurfaceMask = 0x000F0000,
 };
 
 enum class MetadataType : uint32_t {
   OWNER_UID = 1,
   WINDOW_TYPE = 2,
   TASK_ID = 3,
   MOUSE_CURSOR = 4,
   ACCESSIBILITY_ID = 5,
   OWNER_PID = 6,
   DEQUEUE_TIME = 7,
   GAME_MODE = 8
 };
 
 enum { WINDOW_TYPE_DONT_SCREENSHOT = 441731 };
 
 template <typename any_t>
 struct StrongPointer {
   union {
     any_t* pointer;
     char padding[sizeof(std::max_align_t)];
   };
 
   inline any_t* operator->() const { return pointer; }
   inline any_t* get() const { return pointer; }
   inline explicit operator bool() const { return nullptr != pointer; }
 };
 
 template <typename enum_t>
 constexpr enum_t operator|(enum_t lhs, enum_t rhs)
   requires std::is_enum_v<enum_t>
 {
   using underlying_t = std::underlying_type_t<enum_t>;
 
   return static_cast<enum_t>(static_cast<underlying_t>(lhs) |
                              static_cast<underlying_t>(rhs));
 }
 
 template <typename enum_t>
 constexpr enum_t operator|=(enum_t& lhs, enum_t rhs)
   requires std::is_enum_v<enum_t>
 {
   return (lhs = lhs | rhs);
 }
 }  // namespace android::anative_window_creator::detail::types
 
 namespace android::anative_window_creator::detail::types::ui {
 /**
  * The following types and structures are adapted from AOSP android::ui.
  */
 
 typedef int64_t nsecs_t;  // nano-seconds
 
 enum class Rotation {
   Rotation0 = 0,
   Rotation90 = 1,
   Rotation180 = 2,
   Rotation270 = 3
 };
 
 enum class DisplayType { DisplayIdMain = 0, DisplayIdHdmi = 1 };
 
 // A LayerStack identifies a Z-ordered group of layers. A layer can only be
 // associated to a single LayerStack, but a LayerStack can be associated to
 // multiple displays, mirroring the same content.
 struct LayerStack {
   uint32_t id = UINT32_MAX;
 };
 
 // A simple value type representing a two-dimensional size.
 struct Size {
   int32_t width = -1;
   int32_t height = -1;
 };
 
 // Transactional state of physical or virtual display. Note that libgui defines
 // android::DisplayState as a superset of android::ui::DisplayState.
 struct DisplayState {
   LayerStack layerStack;
   Rotation orientation = Rotation::Rotation0;
   Size layerStackSpaceRect;
 };
 
 struct DisplayInfo {
   uint32_t w{0};
   uint32_t h{0};
   float xdpi{0};
   float ydpi{0};
   float fps{0};
   float density{0};
   uint8_t orientation{0};
   bool secure{false};
   nsecs_t appVsyncOffset{0};
   nsecs_t presentationDeadline{0};
   uint32_t viewportW{0};
   uint32_t viewportH{0};
 };
 
 struct PhysicalDisplayId {
   uint64_t value;
 };
 }  // namespace android::anative_window_creator::detail::types::ui
 
 namespace android::anative_window_creator::detail::types::apis::libutils {
 namespace generic {
 using RefBase__IncStrong = void (*)(void* thiz, void* id);
 using RefBase__DecStrong = void (*)(void* thiz, void* id);
 
 using String8__Constructor = void* (*)(void* thiz, const char* const string);
 using String8__Destructor = void (*)(void* thiz);
 }  // namespace generic
 }  // namespace android::anative_window_creator::detail::types::apis::libutils
 
 namespace android::anative_window_creator::detail::types::apis::libgui {
 namespace v5_v7 {
 // SurfaceComposerClient::createSurface(
 //         const String8& name,
 //         uint32_t w,
 //         uint32_t h,
 //         PixelFormat format,
 //         uint32_t flags)
 using SurfaceComposerClient__CreateSurface =
     StrongPointer<void> (*)(void* thiz, void* name, uint32_t w, uint32_t h,
                             PixelFormat format, WindowFlags flags);
 
 using SurfaceComposerClient__OpenGlobalTransaction__Static = void (*)();
 using SurfaceComposerClient__CloseGlobalTransaction__Static =
     void (*)(bool synchronous);
 
 using SurfaceControl__SetLayer = void* (*)(void* thiz, int32_t z);
 
 // enum {
 //     // The API number used to indicate the currently connected producer
 //     CURRENTLY_CONNECTED_API = -1,
 //
 //     // The API number used to indicate that no producer is connected
 //     NO_CONNECTED_API        = 0,
 // };
 using Surface__DisConnect = void* (*)(void* thiz, int api);
 }  // namespace v5_v7
 
 namespace v8_v9 {
 // SurfaceComposerClient::createSurface(
 //         const String8& name,
 //         uint32_t w,
 //         uint32_t h,
 //         PixelFormat format,
 //         uint32_t flags,
 //         SurfaceControl* parent,
 //         uint32_t windowType,
 //         uint32_t ownerUid)
 using SurfaceComposerClient__CreateSurface = StrongPointer<void> (*)(
     void* thiz, void* name, uint32_t w, uint32_t h, PixelFormat format,
     WindowFlags flags, void* parent, uint32_t windowType, uint32_t ownerUid);
 }  // namespace v8_v9
 
 namespace v8_v12 {
 using SurfaceComposerClient__Transaction__Apply = int32_t (*)(void* thiz,
                                                               bool synchronous);
 }  // namespace v8_v12
 
 namespace v10 {
 // SurfaceComposerClient::createSurface(const String8& name, uint32_t w,
 // uint32_t h,
 //         PixelFormat format, uint32_t flags,
 //         SurfaceControl* parent,
 //         LayerMetadata metadata)
 using SurfaceComposerClient__CreateSurface = StrongPointer<void> (*)(
     void* thiz, void* name, uint32_t w, uint32_t h, PixelFormat format,
     WindowFlags flags, void* parent, void* metadata);
 }  // namespace v10
 
 namespace v10_v12 {
 // SurfaceComposerClient::Transaction&
 // SurfaceComposerClient::Transaction::setInputWindowInfo(
 //         const sp<SurfaceControl>& sc,
 //         const InputWindowInfo& info)
 using SurfaceComposerClient__Transaction__SetInputWindowInfo =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl,
               void* inputWindowInfo);
 }  // namespace v10_v12
 
 namespace v11 {
 // SurfaceComposerClient::createSurface(const String8& name, uint32_t w,
 // uint32_t h,
 //         PixelFormat format, uint32_t flags,
 //         SurfaceControl* parent,
 //         LayerMetadata metadata,
 //         uint32_t* outTransformHint)
 using SurfaceComposerClient__CreateSurface =
     StrongPointer<void> (*)(void* thiz, void* name, uint32_t w, uint32_t h,
                             PixelFormat format, WindowFlags flags, void* parent,
                             void* metadata, uint32_t* outTransformHint);
 }  // namespace v11
 
 namespace v12_v13 {
 // SurfaceComposerClient::createSurface(const String8& name, uint32_t w,
 // uint32_t h,
 //         PixelFormat format, uint32_t flags,
 //         const sp<IBinder>& parentHandle,
 //         LayerMetadata metadata,
 //         uint32_t* outTransformHint)
 using SurfaceComposerClient__CreateSurface = StrongPointer<void> (*)(
     void* thiz, void* name, uint32_t w, uint32_t h, PixelFormat format,
     WindowFlags flags, void** parentHandle, void* metadata,
     uint32_t* outTransformHint);
 }  // namespace v12_v13
 
 namespace v13_v15 {
 // SurfaceComposerClient::Transaction&
 // SurfaceComposerClient::Transaction::setInputWindowInfo(
 //         const sp<SurfaceControl>& sc, const WindowInfo& info)
 using SurfaceComposerClient__Transaction__SetInputWindowInfo =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl,
               void* windowInfo);
 }  // namespace v13_v15
 
 namespace v13_infinite {
 using SurfaceComposerClient__Transaction__Apply = int32_t (*)(void* thiz,
                                                               bool synchronous,
                                                               bool oneWay);
 }  // namespace v13_infinite
 
 namespace v14_infinite {
 // SurfaceComposerClient::createSurface(const String8& name, uint32_t w,
 // uint32_t h,
 //         PixelFormat format, int32_t flags,
 //         const sp<IBinder>& parentHandle,
 //         LayerMetadata metadata,
 //         uint32_t* outTransformHint)
 using SurfaceComposerClient__CreateSurface = StrongPointer<void> (*)(
     void* thiz, void* name, uint32_t w, uint32_t h, PixelFormat format,
     WindowFlags flags, void** parentHandle, void* metadata,
     uint32_t* outTransformHint);
 }  // namespace v14_infinite
 
 namespace v16_infinite {
 // SurfaceComposerClient::Transaction&
 // SurfaceComposerClient::Transaction::setInputWindowInfo(
 //         const sp<SurfaceControl>& sc, sp<WindowInfoHandle> info)
 using SurfaceComposerClient__Transaction__SetInputWindowInfo =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl,
               void* windowInfoHandle);
 }  // namespace v16_infinite
 
 namespace generic {
 using LayerMetadata__Constructor = void (*)(void* thiz);
 using LayerMetadata__SetInt32 = void (*)(void* thiz, MetadataType key,
                                          int32_t value);
 
 using SurfaceComposerClient__Constructor = void* (*)(void* thiz);
 using SurfaceComposerClient__MirrorSurface =
     StrongPointer<void> (*)(void* thiz, void* mirrorFromSurface);
 using SurfaceComposerClient__MirrorSurfaceWithParent =
     StrongPointer<void> (*)(void* thiz, void* mirrorFromSurface, void* parent);
 // Static
 using SurfaceComposerClient__GetInternalDisplayToken__Static =
     StrongPointer<void> (*)();
 using SurfaceComposerClient__GetBuiltInDisplay__Static =
     StrongPointer<void> (*)(ui::DisplayType type);
 using SurfaceComposerClient__GetDisplayState__Static =
     int32_t (*)(StrongPointer<void>& display, ui::DisplayState* displayState);
 using SurfaceComposerClient__GetDisplayInfo__Static =
     int32_t (*)(StrongPointer<void>& display, ui::DisplayInfo* displayInfo);
 using SurfaceComposerClient__GetPhysicalDisplayIds__Static =
     std::vector<ui::PhysicalDisplayId> (*)();
 using SurfaceComposerClient__GetPhysicalDisplayToken__Static =
     StrongPointer<void> (*)(ui::PhysicalDisplayId displayId);
 
 using SurfaceComposerClient__Transaction__CopyConstructor =
     void* (*)(void* thiz, void* other);
 using SurfaceComposerClient__Transaction__Constructor = void* (*)(void* thiz);
 using SurfaceComposerClient__Transaction__SetLayer =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl, int32_t z);
 using SurfaceComposerClient__Transaction__SetTrustedOverlay =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl,
               bool isTrustedOverlay);
 using SurfaceComposerClient__Transaction__SetLayerStack =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl,
               uint32_t layerStack);
 using SurfaceComposerClient__Transaction__Show =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl);
 using SurfaceComposerClient__Transaction__Hide =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl);
 using SurfaceComposerClient__Transaction__Reparent =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl,
               StrongPointer<void>& newParentHandle);
 using SurfaceComposerClient__Transaction__SetMatrix =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl, float dsdx,
               float dtdx, float dtdy, float dsdy);
 using SurfaceComposerClient__Transaction__SetPosition =
     void* (*)(void* thiz, StrongPointer<void>& surfaceControl, float x,
               float y);
 
 using SurfaceControl__GetSurface = StrongPointer<void> (*)(void* thiz);
 using SurfaceControl__DisConnect = void (*)(void* thiz);
 using SurfaceControl__GetParentingLayer = StrongPointer<void> (*)(void* thiz);
 }  // namespace generic
 }  // namespace android::anative_window_creator::detail::types::apis::libgui
 
 namespace android::anative_window_creator::detail::types::apis {
 struct ApiDescriptor {
   size_t minVersion;
   size_t maxVersion;
   void** storeToTarget;
   const char* apiSignature;
   // When true, a failed resolve is tolerated (the pointer stays null) instead
   // of throwing. Used for symbols whose presence varies across ROMs of the
   // same OS version, where the caller selects an alternative at runtime.
   bool optional = false;
 
   bool IsSupported(size_t currentVersion) const {
     return currentVersion >= minVersion && currentVersion <= maxVersion;
   }
 };
 }  // namespace android::anative_window_creator::detail::types::apis
 
 namespace android::anative_window_creator::detail::apis {
 namespace libutils {
 struct RefBase {
   struct ApiTable {
     void* IncStrong;
     void* DecStrong;
   };
 
   inline static ApiTable Api;
 };
 
 struct String8 {
   struct ApiTable {
     void* Constructor;
     void* Destructor;
   };
 
   inline static ApiTable Api;
 };
 }  // namespace libutils
 
 namespace libgui {
 struct LayerMetadata {
   struct ApiTable {
     void* Constructor;
     void* SetInt32;
   };
 
   inline static ApiTable Api;
 };
 
 struct SurfaceComposerClient {
   struct Transaction;
 
   struct ApiTable {
     void* Constructor;
     void* CreateSurface;
     void* MirrorSurface;
     void* MirrorSurfaceWithParent;
     void* GetInternalDisplayToken;
     void* GetBuiltInDisplay;
     void* GetDisplayState;
     void* GetDisplayInfo;
     void* GetPhysicalDisplayIds;
     void* GetPhysicalDisplayToken;
 
     void* OpenGlobalTransaction;
     void* CloseGlobalTransaction;
   };
 
   inline static ApiTable Api;
 };
 
 struct SurfaceComposerClient::Transaction {
   struct ApiTable {
     void* CopyConstructor;
     void* Constructor;
     void* SetLayer;
     void* SetTrustedOverlay;
     void* SetLayerStack;
     void* Show;
     void* Hide;
     void* Reparent;
     void* SetMatrix;
     void* SetPosition;
     void* SetInputWindowInfo;
     void* Apply;
   };
 
   inline static ApiTable Api;
 };
 
 struct SurfaceControl {
   struct ApiTable {
     void* GetSurface;
     void* DisConnect;
     void* GetParentingLayer;
 
     void* SetLayer;
   };
 
   inline static ApiTable Api;
 };
 
 struct Surface {
   struct ApiTable {
     void* DisConnect;
   };
 
   inline static ApiTable Api;
 };
 }  // namespace libgui
 }  // namespace android::anative_window_creator::detail::apis
 
 namespace android::anative_window_creator::detail::compat {
 constexpr size_t SupportedMinVersion = 5;
 
 template <size_t length>
 struct ApiInvokeDescriptor {
   size_t api;
   size_t version;
 
   consteval size_t DataHash(const std::string_view& data) const {
     constexpr size_t fnvOffsetBasis = 0x811C9DC5ull;
     constexpr size_t fnvPrime = 0x1000193ull;
     size_t hash = fnvOffsetBasis;
 
     for (size_t i = 0; i < data.size(); ++i) {
       hash ^= static_cast<size_t>(data[i]);
       hash *= fnvPrime;
     }
 
     return hash;
   }
 
   consteval bool operator==(const char* rhs) const {
     return api == DataHash(rhs);
   }
 
   consteval ApiInvokeDescriptor(const char (&apiInvokeName)[length]) {
     const std::string_view apiInvokeNameView{apiInvokeName};
     const auto invokeNameDelimiter = apiInvokeNameView.find("@");
 
     if (std::string_view::npos == invokeNameDelimiter) {
       api = DataHash(apiInvokeName);
       version = SupportedMinVersion;
     } else {
       if ('v' != apiInvokeNameView[invokeNameDelimiter + 1]) {
         (::fprintf(stderr, "[native_window] %s\n",
                    "Invalid version string, must start with 'v'"),
          ::abort());
       }
 
       const auto invokeName = apiInvokeNameView.substr(0, invokeNameDelimiter);
       const auto invokeVersion =
           apiInvokeNameView.substr(invokeNameDelimiter + 2);
 
       api = DataHash(invokeName);
 
       version = 0;
       for (char c : invokeVersion) {
         if (c < '0' || c > '9') {
           (::fprintf(stderr, "[native_window] %s\n",
                      "Invalid version string, must be digits only"),
            ::abort());
         }
 
         version = version * 10 + (c - '0');
       }
     }
   }
 };
 
 template <ApiInvokeDescriptor descriptor>
 constexpr auto ApiInvoker() {
   // libutils
   if constexpr ("RefBase::IncStrong" == descriptor) {
     return reinterpret_cast<types::apis::libutils::generic::RefBase__IncStrong>(
         apis::libutils::RefBase::Api.IncStrong);
   }
   if constexpr ("RefBase::DecStrong" == descriptor) {
     return reinterpret_cast<types::apis::libutils::generic::RefBase__DecStrong>(
         apis::libutils::RefBase::Api.DecStrong);
   }
 
   if constexpr ("String8::Constructor" == descriptor) {
     return reinterpret_cast<
         types::apis::libutils::generic::String8__Constructor>(
         apis::libutils::String8::Api.Constructor);
   }
   if constexpr ("String8::Destructor" == descriptor) {
     return reinterpret_cast<
         types::apis::libutils::generic::String8__Destructor>(
         apis::libutils::String8::Api.Destructor);
   }
 
   // libgui
   if constexpr ("LayerMetadata::Constructor" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::LayerMetadata__Constructor>(
         apis::libgui::LayerMetadata::Api.Constructor);
   }
   if constexpr ("LayerMetadata::SetInt32" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::LayerMetadata__SetInt32>(
         apis::libgui::LayerMetadata::Api.SetInt32);
   }
 
   if constexpr ("SurfaceControl::GetSurface" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceControl__GetSurface>(
         apis::libgui::SurfaceControl::Api.GetSurface);
   }
   if constexpr ("SurfaceControl::DisConnect" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceControl__DisConnect>(
         apis::libgui::SurfaceControl::Api.DisConnect);
   }
   if constexpr ("SurfaceControl::GetParentingLayer" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceControl__GetParentingLayer>(
         apis::libgui::SurfaceControl::Api.GetParentingLayer);
   }
   if constexpr ("SurfaceControl::SetLayer" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::v5_v7::SurfaceControl__SetLayer>(
         apis::libgui::SurfaceControl::Api.SetLayer);
   }
 
   if constexpr ("Surface::DisConnect" == descriptor) {
     return reinterpret_cast<types::apis::libgui::v5_v7::Surface__DisConnect>(
         apis::libgui::Surface::Api.DisConnect);
   }
 
   if constexpr ("SurfaceComposerClient::Transaction::Constructor" ==
                 descriptor) {
     if constexpr (12 > descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::generic::
               SurfaceComposerClient__Transaction__CopyConstructor>(
           apis::libgui::SurfaceComposerClient::Transaction::Api
               .CopyConstructor);
     } else {
       return reinterpret_cast<
           types::apis::libgui::generic::
               SurfaceComposerClient__Transaction__Constructor>(
           apis::libgui::SurfaceComposerClient::Transaction::Api.Constructor);
     }
   }
   if constexpr ("SurfaceComposerClient::Transaction::SetLayer" == descriptor) {
     return reinterpret_cast<types::apis::libgui::generic::
                                 SurfaceComposerClient__Transaction__SetLayer>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.SetLayer);
   }
   if constexpr ("SurfaceComposerClient::Transaction::SetLayerStack" ==
                 descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__Transaction__SetLayerStack>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.SetLayerStack);
   }
   if constexpr ("SurfaceComposerClient::Transaction::SetTrustedOverlay" ==
                 descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__Transaction__SetTrustedOverlay>(
         apis::libgui::SurfaceComposerClient::Transaction::Api
             .SetTrustedOverlay);
   }
   if constexpr ("SurfaceComposerClient::Transaction::Show" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceComposerClient__Transaction__Show>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.Show);
   }
   if constexpr ("SurfaceComposerClient::Transaction::Hide" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceComposerClient__Transaction__Hide>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.Hide);
   }
   if constexpr ("SurfaceComposerClient::Transaction::Reparent" == descriptor) {
     return reinterpret_cast<types::apis::libgui::generic::
                                 SurfaceComposerClient__Transaction__Reparent>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.Reparent);
   }
   if constexpr ("SurfaceComposerClient::Transaction::SetMatrix" == descriptor) {
     return reinterpret_cast<types::apis::libgui::generic::
                                 SurfaceComposerClient__Transaction__SetMatrix>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.SetMatrix);
   }
   if constexpr ("SurfaceComposerClient::Transaction::SetPosition" ==
                 descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__Transaction__SetPosition>(
         apis::libgui::SurfaceComposerClient::Transaction::Api.SetPosition);
   }
   if constexpr ("SurfaceComposerClient::Transaction::SetInputWindowInfo" ==
                 descriptor) {
     if constexpr (10 <= descriptor.version && 12 >= descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v10_v12::
               SurfaceComposerClient__Transaction__SetInputWindowInfo>(
           apis::libgui::SurfaceComposerClient::Transaction::Api
               .SetInputWindowInfo);
     } else if constexpr (13 <= descriptor.version && 15 >= descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v13_v15::
               SurfaceComposerClient__Transaction__SetInputWindowInfo>(
           apis::libgui::SurfaceComposerClient::Transaction::Api
               .SetInputWindowInfo);
     } else if constexpr (16 <= descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v16_infinite::
               SurfaceComposerClient__Transaction__SetInputWindowInfo>(
           apis::libgui::SurfaceComposerClient::Transaction::Api
               .SetInputWindowInfo);
     }
   }
   if constexpr ("SurfaceComposerClient::Transaction::Apply" == descriptor) {
     if constexpr (8 <= descriptor.version && 12 >= descriptor.version) {
       return reinterpret_cast<types::apis::libgui::v8_v12::
                                   SurfaceComposerClient__Transaction__Apply>(
           apis::libgui::SurfaceComposerClient::Transaction::Api.Apply);
     } else if constexpr (13 <= descriptor.version) {
       return reinterpret_cast<types::apis::libgui::v13_infinite::
                                   SurfaceComposerClient__Transaction__Apply>(
           apis::libgui::SurfaceComposerClient::Transaction::Api.Apply);
     }
   }
 
   if constexpr ("SurfaceComposerClient::Constructor" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceComposerClient__Constructor>(
         apis::libgui::SurfaceComposerClient::Api.Constructor);
   }
   if constexpr ("SurfaceComposerClient::CreateSurface" == descriptor) {
     if constexpr (5 <= descriptor.version && 7 >= descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v5_v7::SurfaceComposerClient__CreateSurface>(
           apis::libgui::SurfaceComposerClient::Api.CreateSurface);
     } else if constexpr (8 <= descriptor.version && 9 >= descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v8_v9::SurfaceComposerClient__CreateSurface>(
           apis::libgui::SurfaceComposerClient::Api.CreateSurface);
     } else if constexpr (10 == descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v10::SurfaceComposerClient__CreateSurface>(
           apis::libgui::SurfaceComposerClient::Api.CreateSurface);
     } else if constexpr (11 == descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v11::SurfaceComposerClient__CreateSurface>(
           apis::libgui::SurfaceComposerClient::Api.CreateSurface);
     } else if constexpr (12 <= descriptor.version && 13 >= descriptor.version) {
       return reinterpret_cast<
           types::apis::libgui::v12_v13::SurfaceComposerClient__CreateSurface>(
           apis::libgui::SurfaceComposerClient::Api.CreateSurface);
     } else if constexpr (14 <= descriptor.version) {
       return reinterpret_cast<types::apis::libgui::v14_infinite::
                                   SurfaceComposerClient__CreateSurface>(
           apis::libgui::SurfaceComposerClient::Api.CreateSurface);
     }
   }
   if constexpr ("SurfaceComposerClient::MirrorSurface" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::SurfaceComposerClient__MirrorSurface>(
         apis::libgui::SurfaceComposerClient::Api.MirrorSurface);
   }
   if constexpr ("SurfaceComposerClient::MirrorSurfaceWithParent" ==
                 descriptor) {
     return reinterpret_cast<types::apis::libgui::generic::
                                 SurfaceComposerClient__MirrorSurfaceWithParent>(
         apis::libgui::SurfaceComposerClient::Api.MirrorSurfaceWithParent);
   }
 
   if constexpr ("SurfaceComposerClient::GetBuiltInDisplay" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__GetBuiltInDisplay__Static>(
         apis::libgui::SurfaceComposerClient::Api.GetBuiltInDisplay);
   }
   if constexpr ("SurfaceComposerClient::GetInternalDisplayToken" ==
                 descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__GetInternalDisplayToken__Static>(
         apis::libgui::SurfaceComposerClient::Api.GetInternalDisplayToken);
   }
   if constexpr ("SurfaceComposerClient::GetPhysicalDisplayIds" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__GetPhysicalDisplayIds__Static>(
         apis::libgui::SurfaceComposerClient::Api.GetPhysicalDisplayIds);
   }
   if constexpr ("SurfaceComposerClient::GetPhysicalDisplayToken" ==
                 descriptor) {
     return reinterpret_cast<
         types::apis::libgui::generic::
             SurfaceComposerClient__GetPhysicalDisplayToken__Static>(
         apis::libgui::SurfaceComposerClient::Api.GetPhysicalDisplayToken);
   }
   if constexpr ("SurfaceComposerClient::GetDisplayState" == descriptor) {
     return reinterpret_cast<types::apis::libgui::generic::
                                 SurfaceComposerClient__GetDisplayState__Static>(
         apis::libgui::SurfaceComposerClient::Api.GetDisplayState);
   }
   if constexpr ("SurfaceComposerClient::GetDisplayInfo" == descriptor) {
     return reinterpret_cast<types::apis::libgui::generic::
                                 SurfaceComposerClient__GetDisplayInfo__Static>(
         apis::libgui::SurfaceComposerClient::Api.GetDisplayInfo);
   }
 
   if constexpr ("SurfaceComposerClient::OpenGlobalTransaction" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::v5_v7::
             SurfaceComposerClient__OpenGlobalTransaction__Static>(
         apis::libgui::SurfaceComposerClient::Api.OpenGlobalTransaction);
   }
   if constexpr ("SurfaceComposerClient::CloseGlobalTransaction" == descriptor) {
     return reinterpret_cast<
         types::apis::libgui::v5_v7::
             SurfaceComposerClient__CloseGlobalTransaction__Static>(
         apis::libgui::SurfaceComposerClient::Api.CloseGlobalTransaction);
   }
 }
 
 }  // namespace android::anative_window_creator::detail::compat
 
 namespace android::anative_window_creator::detail::compat {
 // NOTE: No default implementation
 extern size_t SystemVersion;
 
 struct String8 {
   char data[1024];
 
   String8(const char* const string) {
     ApiInvoker<"String8::Constructor">()(data, string);
   }
 
   ~String8() { ApiInvoker<"String8::Destructor">()(data); }
 
   operator void*() { return reinterpret_cast<void*>(data); }
 };
 
 struct LayerMetadata {
   char data[1024];
 
   LayerMetadata() {
     if (9 < SystemVersion) {
       ApiInvoker<"LayerMetadata::Constructor@v10">()(data);
     }
   }
 
   void SetInt32(types::MetadataType key, int32_t value) {
     if (9 < SystemVersion) {
       ApiInvoker<"LayerMetadata::SetInt32@v10">()(data, key, value);
     }
   }
 
   operator void*() {
     if (9 < SystemVersion) {
       return reinterpret_cast<void*>(data);
     } else {
       return nullptr;
     }
   }
 };
 
 struct Surface {};
 
 struct SurfaceControl {
   void* data;
   int32_t width, height;
   bool skipScreenshot;
 
   SurfaceControl(void* data = nullptr)
       : data(data), width(0), height(0), skipScreenshot(false) {}
   SurfaceControl(void* data, int32_t width, int32_t height, bool skipScreenshot)
       : data(data),
         width(width),
         height(height),
         skipScreenshot(skipScreenshot) {}
 
   operator bool() const { return nullptr != data; }
 
   operator types::StrongPointer<void>&() {
     // thread_local: the render thread and the mirror thread both call this
     // independently; a plain static would be a data race between the two.
     static thread_local types::StrongPointer<void> result;
 
     result.pointer = data;
 
     return result;
   }
 
   Surface* GetSurface() {
     if (nullptr == data) {
       return nullptr;
     }
 
     auto result = ApiInvoker<"SurfaceControl::GetSurface">()(data);
 
     return reinterpret_cast<Surface*>(reinterpret_cast<size_t>(result.pointer) +
                                       sizeof(std::max_align_t) / 2);
   }
 
   types::StrongPointer<void> GetParentingLayer() {
     if (nullptr == data) {
       return {};
     }
 
     if (12 > SystemVersion) {
       types::StrongPointer<void> result;
 
       result.pointer = data;
 
       return result;
     }
 
     return ApiInvoker<"SurfaceControl::GetParentingLayer">()(data);
   }
 
   void DisConnect() {
     if (nullptr == data) {
       return;
     }
 
     ApiInvoker<"SurfaceControl::DisConnect">()(data);
   }
 
   void SetLayer(int32_t z) {
     if (nullptr == data || 8 < SystemVersion) {
       return;
     }
 
     ApiInvoker<"SurfaceControl::SetLayer@v8">()(data, z);
   }
 
   void DestroySurface(Surface* surface) {
     if (nullptr == data || nullptr == surface) {
       return;
     }
 
     ApiInvoker<"RefBase::DecStrong">()(
         reinterpret_cast<Surface*>(reinterpret_cast<size_t>(surface) -
                                    sizeof(std::max_align_t) / 2),
         this);
     if (7 > SystemVersion) {
       ApiInvoker<"Surface::DisConnect@v6">()(
           reinterpret_cast<Surface*>(reinterpret_cast<size_t>(surface) -
                                      sizeof(std::max_align_t) / 2),
           -1);
     } else {
       DisConnect();
     }
     ApiInvoker<"RefBase::DecStrong">()(data, this);
   }
 };
 
 struct SurfaceComposerClientTransaction {
   char data[1024];
 
   SurfaceComposerClientTransaction() {
     if (12 > SystemVersion) {
       ApiInvoker<"SurfaceComposerClient::Transaction::Constructor@v11">()(data,
                                                                           data);
     } else {
       ApiInvoker<"SurfaceComposerClient::Transaction::Constructor@v12">()(data);
     }
   }
 
   void* SetLayer(types::StrongPointer<void>& surfaceControl, int32_t z) {
     return ApiInvoker<"SurfaceComposerClient::Transaction::SetLayer">()(
         data, surfaceControl, z);
   }
 
   void* SetLayerStack(types::StrongPointer<void>& surfaceControl,
                       uint32_t layerStack) {
     return ApiInvoker<"SurfaceComposerClient::Transaction::SetLayerStack">()(
         data, surfaceControl, layerStack);
   }
 
   void* SetTrustedOverlay(types::StrongPointer<void>& surfaceControl,
                           bool isTrustedOverlay) {
     return ApiInvoker<
         "SurfaceComposerClient::Transaction::SetTrustedOverlay">()(
         data, surfaceControl, isTrustedOverlay);
   }
 
   void Show(types::StrongPointer<void>& surfaceControl) {
     ApiInvoker<"SurfaceComposerClient::Transaction::Show">()(data,
                                                              surfaceControl);
   }
 
   void Hide(types::StrongPointer<void>& surfaceControl) {
     ApiInvoker<"SurfaceComposerClient::Transaction::Hide">()(data,
                                                              surfaceControl);
   }
 
   void Reparent(types::StrongPointer<void>& surfaceControl,
                 types::StrongPointer<void>& newParentHandle) {
     ApiInvoker<"SurfaceComposerClient::Transaction::Reparent">()(
         data, surfaceControl, newParentHandle);
   }
 
   void SetMatrix(types::StrongPointer<void>& surfaceControl, float dsdx,
                  float dtdx, float dsdy, float dtdy) {
     ApiInvoker<"SurfaceComposerClient::Transaction::SetMatrix">()(
         data, surfaceControl, dsdx, dtdx, dsdy, dtdy);
   }
 
   void SetPosition(types::StrongPointer<void>& surfaceControl, float x,
                    float y) {
     ApiInvoker<"SurfaceComposerClient::Transaction::SetPosition">()(
         data, surfaceControl, x, y);
   }
 
   void SetInputWindowInfo(types::StrongPointer<void>& surfaceControl,
                           void* windowInfo) {
     ApiInvoker<"SurfaceComposerClient::Transaction::SetInputWindowInfo@v10">()(
         data, surfaceControl, windowInfo);
   }
 
   int32_t Apply(bool synchronous, bool oneWay) {
     if (13 > SystemVersion) {
       return ApiInvoker<"SurfaceComposerClient::Transaction::Apply@v12">()(
           data, synchronous);
     } else {
       return ApiInvoker<"SurfaceComposerClient::Transaction::Apply@v13">()(
           data, synchronous, oneWay);
     }
   }
 };
 
 struct SurfaceComposerClient {
   char data[1024];
 
   SurfaceComposerClient() {
     ApiInvoker<"SurfaceComposerClient::Constructor">()(data);
     ApiInvoker<"RefBase::IncStrong">()(data, this);
   }
 
   SurfaceControl CreateSurface(const char* name, int32_t width, int32_t height,
                                types::WindowFlags windowFlags = {},
                                bool skipScreenshot = false) {
     static void* parentHandle = nullptr;
 
     parentHandle = nullptr;
     String8 windowName(name);
     types::PixelFormat pixelFormat = types::PixelFormat::RGBA_8888;
     LayerMetadata layerMetadata{};
 
     types::StrongPointer<void> result{};
     switch (SystemVersion) {
       case 5:
       case 6:
       case 7: {
         result = ApiInvoker<"SurfaceComposerClient::CreateSurface@v7">()(
             data, windowName, width, height, pixelFormat, windowFlags);
         break;
       }
       case 8:
       case 9: {
         uint32_t windowType =
             skipScreenshot ? types::WINDOW_TYPE_DONT_SCREENSHOT : 0;
 
         result = ApiInvoker<"SurfaceComposerClient::CreateSurface@v9">()(
             data, windowName, width, height, pixelFormat, windowFlags,
             parentHandle, windowType, 0);
         break;
       }
       case 10: {
         if (skipScreenshot) {
           layerMetadata.SetInt32(types::MetadataType::WINDOW_TYPE,
                                  types::WINDOW_TYPE_DONT_SCREENSHOT);
         }
 
         result = ApiInvoker<"SurfaceComposerClient::CreateSurface@v10">()(
             data, windowName, width, height, pixelFormat, windowFlags,
             parentHandle, layerMetadata);
         break;
       }
       case 11: {
         if (skipScreenshot) {
           layerMetadata.SetInt32(types::MetadataType::WINDOW_TYPE,
                                  types::WINDOW_TYPE_DONT_SCREENSHOT);
         }
 
         result = ApiInvoker<"SurfaceComposerClient::CreateSurface@v11">()(
             data, windowName, width, height, pixelFormat, windowFlags,
             parentHandle, layerMetadata, nullptr);
         break;
       }
       case 12:
       case 13: {
         using types::operator|=;
 
         if (skipScreenshot) {
           windowFlags |= types::WindowFlags::eSkipScreenshot;
         }
 
         result = ApiInvoker<"SurfaceComposerClient::CreateSurface@v13">()(
             data, windowName, width, height, pixelFormat, windowFlags,
             &parentHandle, layerMetadata, nullptr);
       }
       default: {
         using types::operator|=;
 
         if (skipScreenshot) {
           windowFlags |= types::WindowFlags::eSkipScreenshot;
         }
 
         result = ApiInvoker<"SurfaceComposerClient::CreateSurface@v14">()(
             data, windowName, width, height, pixelFormat, windowFlags,
             &parentHandle, layerMetadata, nullptr);
         break;
       }
     }
 
     if (12 <= SystemVersion) {
       auto& transaction = GetDefaultTransaction();
       transaction.SetTrustedOverlay(result, true);
       transaction.SetLayer(result, INT_MAX);
       transaction.Apply(false, true);
     } else if (8 >= SystemVersion) {
       OpenGlobalTransaction();
       SurfaceControl{result.get()}.SetLayer(INT_MAX);
       CloseGlobalTransaction(false);
     }
 
     return {result.get(), width, height, skipScreenshot};
   }
 
   // Mirrors `surface` (the on-screen overlay) into `layerStack` (a virtual
   // display: recorder, cast, TikTok/YouTube live).  Returns the mirror CLONE
   // wrapped in a SurfaceControl so the caller can apply the scale/rotation
   // matrix directly to it — a mirror layer renders the source's layer tree at
   // the SOURCE resolution, so a transform on its parent container only clips,
   // it does not rescale.  The transform MUST land on the clone itself.
   //
   // The root container is created at the TARGET (virtual-display) dimensions
   // so it covers the whole foreign display and never clips the (possibly
   // rotated) clone.  targetWidth/targetHeight come from the virtual display's
   // mCurrentLayerStackRect.
   SurfaceControl MirrorSurface(SurfaceControl& surface, uint32_t layerStack,
                                int32_t targetWidth, int32_t targetHeight) {
     if (14 > compat::SystemVersion) {
       return {};
     }
     if (surface.skipScreenshot) {
       LogInfo("[=] Surface not need mirror: skipScreenshot is true");
       return {};
     }
     if (0 == surface.width || 0 == surface.height) {
       LogInfo("[=] Surface not need mirror: width or height is 0");
       return {};
     }
 
     const int32_t root_w = targetWidth > 0 ? targetWidth : surface.width;
     const int32_t root_h = targetHeight > 0 ? targetHeight : surface.height;
 
     // Stack-allocated name — avoids the two std::string heap allocs that
     // "MirrorRoot@" + std::to_string() would trigger.
     char mirror_root_name[32];
     std::snprintf(mirror_root_name, sizeof(mirror_root_name), "MirrorRoot@%u",
                   layerStack);
     auto mirrorRootSurface = CreateSurface(mirror_root_name, root_w, root_h,
                                            types::WindowFlags::eNoColorFill);
     if (!mirrorRootSurface) {
       LogError("[-] Failed to create mirror root surface: %u", layerStack);
       return {};
     }
 
     auto& transaction = GetDefaultTransaction();
     transaction.SetLayer(mirrorRootSurface, INT_MAX);
     transaction.SetLayerStack(mirrorRootSurface, layerStack);
     transaction.Show(mirrorRootSurface);
     transaction.Apply(false, true);
 
     // Android 16 added a mirrorSurface() overload taking an explicit parent;
     // the returned mirror is already attached, so no reparent is needed. Some
     // Android 16 ROMs (e.g. Red Magic NX799J) still ship only the legacy
     // one-argument overload, so select by which symbol actually resolved
     // rather than by the reported OS version.
     const bool has_parent_overload =
         nullptr !=
         apis::libgui::SurfaceComposerClient::Api.MirrorSurfaceWithParent;
 
     types::StrongPointer<void> mirrorSurface;
     if (has_parent_overload) {
       mirrorSurface =
           ApiInvoker<"SurfaceComposerClient::MirrorSurfaceWithParent">()(
               data, surface.data, mirrorRootSurface.data);
     } else {
       mirrorSurface = ApiInvoker<"SurfaceComposerClient::MirrorSurface@v14">()(
           data, surface.data);
     }
     if (nullptr == mirrorSurface.get()) {
       LogError("[-] Failed to mirror surface: %u", layerStack);
       return {};
     }
 
     transaction.SetLayerStack(mirrorSurface, layerStack);
     transaction.Show(mirrorSurface);
     if (!has_parent_overload) {
       transaction.Reparent(mirrorSurface, mirrorRootSurface);
     }
     transaction.Apply(false, true);
 
     // Store the cleanup handle keyed by the CLONE pointer (the value the
     // caller keeps and transforms).  Erasing it from s_mirror_handles calls
     // DeleteMirrorPair, disconnecting both native surfaces.
     s_mirror_handles.emplace(
         mirrorSurface.get(),
         MirrorHandle{
             new MirrorPair{mirrorSurface.get(), mirrorRootSurface.data},
             &DeleteMirrorPair});
 
     // Return the CLONE — the surface the scale/rotation matrix must target.
     return SurfaceControl{mirrorSurface.get(), root_w, root_h, false};
   }
 
   // Apply a scale+rotation matrix and position in one atomic transaction.
   // dsdx/dtdx/dtdy/dsdy are AOSP native setMatrix parameters.
   void TransformSurface(SurfaceControl& surface, float dsdx, float dtdx,
                         float dtdy, float dsdy, float x, float y) {
     auto& t = GetDefaultTransaction();
     t.SetMatrix(surface, dsdx, dtdx, dtdy, dsdy);
     t.SetPosition(surface, x, y);
     t.Apply(false, true);
   }
 
   bool GetDisplayInfo(types::ui::DisplayState* displayInfo) {
     types::StrongPointer<void> defaultDisplay;
 
     if (9 >= SystemVersion) {
       defaultDisplay =
           ApiInvoker<"SurfaceComposerClient::GetBuiltInDisplay@v9">()(
               types::ui::DisplayType::DisplayIdMain);
     } else {
       if (14 > SystemVersion) {
         defaultDisplay = ApiInvoker<
             "SurfaceComposerClient::GetInternalDisplayToken@v13">()();
       } else {
         auto displayIds =
             ApiInvoker<"SurfaceComposerClient::GetPhysicalDisplayIds@v14">()();
         if (displayIds.empty()) {
           return false;
         }
 
         defaultDisplay =
             ApiInvoker<"SurfaceComposerClient::GetPhysicalDisplayToken@v14">()(
                 displayIds[0]);
       }
     }
 
     if (nullptr == defaultDisplay.get()) {
       return false;
     }
 
     if (11 <= SystemVersion) {
       return 0 == ApiInvoker<"SurfaceComposerClient::GetDisplayState@v11">()(
                       defaultDisplay, displayInfo);
     } else {
       types::ui::DisplayInfo realDisplayInfo{};
       if (0 != ApiInvoker<"SurfaceComposerClient::GetDisplayInfo@v10">()(
                    defaultDisplay, &realDisplayInfo)) {
         return false;
       }
 
       displayInfo->layerStackSpaceRect.width = realDisplayInfo.w;
       displayInfo->layerStackSpaceRect.height = realDisplayInfo.h;
       displayInfo->orientation =
           static_cast<types::ui::Rotation>(realDisplayInfo.orientation);
 
       return true;
     }
   }
 
   void OpenGlobalTransaction() {
     ApiInvoker<"SurfaceComposerClient::OpenGlobalTransaction@v7">()();
   }
 
   void CloseGlobalTransaction(bool synchronous) {
     ApiInvoker<"SurfaceComposerClient::CloseGlobalTransaction@v7">()(
         synchronous);
   }
 
   SurfaceComposerClientTransaction& GetDefaultTransaction() {
     static SurfaceComposerClientTransaction transaction;
 
     return transaction;
   }
 
   // ---- Mirror surface ownership -------------------------------------------
   // Cleanup handle for a (mirrorClone, mirrorRoot) surface pair.  Keyed by
   // the root surface data pointer so ProcessMirrorDisplay can release native
   // SurfaceFlinger resources when a virtual display disconnects.
   using MirrorPair = std::pair<void*, void*>;
   using MirrorHandle = std::unique_ptr<MirrorPair, void (*)(MirrorPair*)>;
 
   static void DeleteMirrorPair(MirrorPair* p) {
     SurfaceControl fake;
     ApiInvoker<"SurfaceControl::DisConnect@v14">()(p->first);
     ApiInvoker<"RefBase::DecStrong">()(p->first, fake.data);
     ApiInvoker<"SurfaceControl::DisConnect@v14">()(p->second);
     ApiInvoker<"RefBase::DecStrong">()(p->second, fake.data);
     delete p;
   }
 
   // Outlives all MirrorSurface() calls; erasing an entry disconnects and
   // releases the native surface pair via DeleteMirrorPair.
   inline static std::unordered_map<void*, MirrorHandle> s_mirror_handles;
 };
 }  // namespace android::anative_window_creator::detail::compat
 
 namespace android::anative_window_creator::detail {
 struct ApiTableDescriptor {
   static auto GetDefaultDescriptors() {
     using namespace android::anative_window_creator::detail::types::apis;
 
     return std::make_pair(
         // libutils
         std::to_array({
             // RefBase
             ApiDescriptor{5, UINT_MAX, &apis::libutils::RefBase::Api.IncStrong,
                           "_ZNK7android7RefBase9incStrongEPKv"},
             ApiDescriptor{5, UINT_MAX, &apis::libutils::RefBase::Api.DecStrong,
                           "_ZNK7android7RefBase9decStrongEPKv"},
 
             // String8
             ApiDescriptor{5, UINT_MAX,
                           &apis::libutils::String8::Api.Constructor,
                           "_ZN7android7String8C2EPKc"},
             ApiDescriptor{5, UINT_MAX, &apis::libutils::String8::Api.Destructor,
                           "_ZN7android7String8D2Ev"},
         }),
         // libgui
         std::to_array({
             // LayerMetadata
             ApiDescriptor{10, 13, &apis::libgui::LayerMetadata::Api.Constructor,
                           "_ZN7android13LayerMetadataC2Ev"},
             ApiDescriptor{14, UINT_MAX,
                           &apis::libgui::LayerMetadata::Api.Constructor,
                           "_ZN7android3gui13LayerMetadataC2Ev"},
 
             ApiDescriptor{10, 13, &apis::libgui::LayerMetadata::Api.SetInt32,
                           "_ZN7android13LayerMetadata8setInt32Eji"},
             ApiDescriptor{14, UINT_MAX,
                           &apis::libgui::LayerMetadata::Api.SetInt32,
                           "_ZN7android3gui13LayerMetadata8setInt32Eji"},
 
             // SurfaceComposerClient
             ApiDescriptor{5, UINT_MAX,
                           &apis::libgui::SurfaceComposerClient::Api.Constructor,
                           "_ZN7android21SurfaceComposerClientC2Ev"},
 
             ApiDescriptor{
                 5, 7, &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8Ejjij"},
             ApiDescriptor{
                 8, 8, &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8EjjijPNS_14SurfaceControlEjj"},
             ApiDescriptor{
                 9, 9, &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8EjjijPNS_14SurfaceControlEii"},
             ApiDescriptor{
                 10, 10, &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataE"},
             ApiDescriptor{
                 11, 11, &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataEPj"},
             ApiDescriptor{
                 12, 13, &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8EjjijRKNS_2spINS_7IBinderEEENS_13LayerMetadataEPj"},
             ApiDescriptor{
                 14, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Api.CreateSurface,
                 "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_"
                 "7String8EjjiiRKNS_2spINS_7IBinderEEENS_"
                 "3gui13LayerMetadataEPj"},
 
             // Legacy one-argument mirrorSurface(). Present on 11..15 and still
             // shipped by some Android 16 ROMs (e.g. Red Magic NX799J) that
             // lack the explicit-parent overload below, so allow it up to the
             // latest version and tolerate its absence.
             ApiDescriptor{
                 11, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Api.MirrorSurface,
                 "_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_"
                 "14SurfaceControlE",
                 true},
 
             // Android 16 explicit-parent mirrorSurface(). Optional: ROMs that
             // still expose only the legacy overload above fall back to it.
             ApiDescriptor{
                 16, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Api
                      .MirrorSurfaceWithParent,
                 "_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_"
                 "14SurfaceControlES2_",
                 true},
 
             ApiDescriptor{
                 5, 9,
                 &apis::libgui::SurfaceComposerClient::Api.GetBuiltInDisplay,
                 "_ZN7android21SurfaceComposerClient17getBuiltInDisplayEi"},
             ApiDescriptor{10, 13,
                           &apis::libgui::SurfaceComposerClient::Api
                                .GetInternalDisplayToken,
                           "_ZN7android21SurfaceComposerClient23getInternalDispl"
                           "ayTokenEv"},
             ApiDescriptor{
                 10, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Api.GetPhysicalDisplayIds,
                 "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv"},
             ApiDescriptor{12, UINT_MAX,
                           &apis::libgui::SurfaceComposerClient::Api
                                .GetPhysicalDisplayToken,
                           "_ZN7android21SurfaceComposerClient23getPhysicalDispl"
                           "ayTokenENS_17PhysicalDisplayIdE"},
 
             ApiDescriptor{
                 5, 11, &apis::libgui::SurfaceComposerClient::Api.GetDisplayInfo,
                 "_ZN7android21SurfaceComposerClient14getDisplayInfoERKNS_"
                 "2spINS_7IBinderEEEPNS_11DisplayInfoE"},
             ApiDescriptor{
                 11, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Api.GetDisplayState,
                 "_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_"
                 "2spINS_7IBinderEEEPNS_2ui12DisplayStateE"},
 
             // SurfaceComposerClient::Transaction
             ApiDescriptor{
                 11, 11,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api
                      .CopyConstructor,
                 "_ZN7android21SurfaceComposerClient11TransactionC2ERKS1_"},
             ApiDescriptor{
                 12, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api
                      .Constructor,
                 "_ZN7android21SurfaceComposerClient11TransactionC2Ev"},
             ApiDescriptor{
                 9, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api.SetLayer,
                 "_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_"
                 "2spINS_14SurfaceControlEEEi"},
             ApiDescriptor{12, UINT_MAX,
                           &apis::libgui::SurfaceComposerClient::Transaction::Api
                                .SetTrustedOverlay,
                           "_ZN7android21SurfaceComposerClient11Transaction17set"
                           "TrustedOverlayERKNS_2spINS_14SurfaceControlEEEb"},
 
             ApiDescriptor{
                 9, 12,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api.Apply,
                 "_ZN7android21SurfaceComposerClient11Transaction5applyEb"},
             ApiDescriptor{
                 13, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api.Apply,
                 "_ZN7android21SurfaceComposerClient11Transaction5applyEbb"},
 
             ApiDescriptor{
                 13, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api
                      .SetLayerStack,
                 "_ZN7android21SurfaceComposerClient11Transaction13setLayerStack"
                 "ERKNS_2spINS_14SurfaceControlEEENS_2ui10LayerStackE"},
             ApiDescriptor{
                 9, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api.Show,
                 "_ZN7android21SurfaceComposerClient11Transaction4showERKNS_"
                 "2spINS_14SurfaceControlEEE"},
             ApiDescriptor{
                 9, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api.Hide,
                 "_ZN7android21SurfaceComposerClient11Transaction4hideERKNS_"
                 "2spINS_14SurfaceControlEEE"},
             ApiDescriptor{
                 12, UINT_MAX,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api.Reparent,
                 "_ZN7android21SurfaceComposerClient11Transaction8reparentERKNS_"
                 "2spINS_14SurfaceControlEEES6_"},
 
             ApiDescriptor{9, UINT_MAX,
                           &apis::libgui::SurfaceComposerClient::Transaction::Api
                                .SetMatrix,
                           "_ZN7android21SurfaceComposerClient11Transaction9setM"
                           "atrixERKNS_2spINS_14SurfaceControlEEEffff"},
             ApiDescriptor{9, UINT_MAX,
                           &apis::libgui::SurfaceComposerClient::Transaction::Api
                                .SetPosition,
                           "_ZN7android21SurfaceComposerClient11Transaction11set"
                           "PositionERKNS_2spINS_14SurfaceControlEEEff"},
 
             ApiDescriptor{
                 10, 12,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api
                      .SetInputWindowInfo,
                 "_ZN7android21SurfaceComposerClient11Transaction18setInputWindo"
                 "wInfoERKNS_2spINS_14SurfaceControlEEERKNS_15InputWindowInfoE"},
             ApiDescriptor{
                 13, 15,
                 &apis::libgui::SurfaceComposerClient::Transaction::Api
                      .SetInputWindowInfo,
                 "_ZN7android21SurfaceComposerClient11Transaction18setInputWindo"
                 "wInfoERKNS_2spINS_14SurfaceControlEEERKNS_3gui10WindowInfoE"},
             ApiDescriptor{16, UINT_MAX,
                           &apis::libgui::SurfaceComposerClient::Transaction::Api
                                .SetInputWindowInfo,
                           "_ZN7android21SurfaceComposerClient11Transaction18set"
                           "InputWindowInfoERKNS_2spINS_14SurfaceControlEEENS2_"
                           "INS_3gui16WindowInfoHandleEEE"},
 
             // SurfaceComposerClient::GlobalTransaction
             ApiDescriptor{
                 5, 8,
                 &apis::libgui::SurfaceComposerClient::Api.OpenGlobalTransaction,
                 "_ZN7android21SurfaceComposerClient21openGlobalTransactionEv"},
             ApiDescriptor{
                 5, 8,
                 &apis::libgui::SurfaceComposerClient::Api
                      .CloseGlobalTransaction,
                 "_ZN7android21SurfaceComposerClient22closeGlobalTransactionEb"},
 
             // SurfaceControl
             ApiDescriptor{5, 11, &apis::libgui::SurfaceControl::Api.GetSurface,
                           "_ZNK7android14SurfaceControl10getSurfaceEv"},
             ApiDescriptor{12, UINT_MAX,
                           &apis::libgui::SurfaceControl::Api.GetSurface,
                           "_ZN7android14SurfaceControl10getSurfaceEv"},
 
             ApiDescriptor{5, 7, &apis::libgui::Surface::Api.DisConnect,
                           "_ZN7android7Surface10disconnectEi"},
             ApiDescriptor{7, UINT_MAX,
                           &apis::libgui::SurfaceControl::Api.DisConnect,
                           "_ZN7android14SurfaceControl10disconnectEv"},
 
             ApiDescriptor{12, UINT_MAX,
                           &apis::libgui::SurfaceControl::Api.GetParentingLayer,
                           "_ZN7android14SurfaceControl17getParentingLayerEv"},
 
             ApiDescriptor{5, 5, &apis::libgui::SurfaceControl::Api.SetLayer,
                           "_ZN7android14SurfaceControl8setLayerEi"},
             ApiDescriptor{6, 7, &apis::libgui::SurfaceControl::Api.SetLayer,
                           "_ZN7android14SurfaceControl8setLayerEj"},
             ApiDescriptor{8, 8, &apis::libgui::SurfaceControl::Api.SetLayer,
                           "_ZN7android14SurfaceControl8setLayerEi"},
         }));
   }
 };
 
 struct ApiResolver {
   struct ResolverImpl {
     void* (*Open)(const char* filename, int flag);
     void* (*Resolve)(void* handle, const char* symbol);
     int (*Close)(void* handle);
 
     ResolverImpl() : Open(dlopen), Resolve(dlsym), Close(dlclose) {}
 
     ResolverImpl(decltype(Open) open, decltype(Resolve) resolve,
                  decltype(Close) close)
         : Open(open), Resolve(resolve), Close(close) {}
   };
 
   static void Resolve(const ResolverImpl& resolver = {}) {
     static bool resolved = false;
 
     if (resolved) {
       return;
     }
 
     std::string systemVersionString(128, 0);
     systemVersionString.resize(__system_property_get(
         "ro.build.version.release", systemVersionString.data()));
     if (!systemVersionString.empty()) {
       compat::SystemVersion = std::stoi(systemVersionString);
     }
 
     if (compat::SupportedMinVersion > compat::SystemVersion) {
       LogError("[!] Unsupported system version: %zu", compat::SystemVersion);
       return;
     }
 
     LogInfo("[*] Starting to resolve symbols for Android %zu",
             compat::SystemVersion);
 
 #ifdef __LP64__
     auto libgui = resolver.Open("/system/lib64/libgui.so", RTLD_LAZY);
     auto libutils = resolver.Open("/system/lib64/libutils.so", RTLD_LAZY);
 #else
     auto libgui = resolver.Open("/system/lib/libgui.so", RTLD_LAZY);
     auto libutils = resolver.Open("/system/lib/libutils.so", RTLD_LAZY);
 #endif
     if (nullptr == libgui || nullptr == libutils) {
       LogError("[!] Failed to open libgui.so or libutils.so");
       (::fprintf(stderr, "[native_window] %s\n",
                  "Failed to open libgui.so or libutils.so"),
        ::abort());
     }
 
     const auto& [libutilsApis, libguiApis] =
         ApiTableDescriptor::GetDefaultDescriptors();
     for (const auto& descriptor : libutilsApis) {
       if (!descriptor.IsSupported(compat::SystemVersion)) {
         continue;
       }
 
       *descriptor.storeToTarget =
           resolver.Resolve(libutils, descriptor.apiSignature);
       if (nullptr == *descriptor.storeToTarget && !descriptor.optional) {
         LogError(
             "[!] Version[Android %zu] [libutils] failed to resolve symbol: %s",
             compat::SystemVersion, descriptor.apiSignature);
         throw std::runtime_error(
             std::string("Failed to resolve [libutils] symbol: ") +
             descriptor.apiSignature);
       }
     }
 
     for (const auto& descriptor : libguiApis) {
       if (!descriptor.IsSupported(compat::SystemVersion)) {
         continue;
       }
 
       *descriptor.storeToTarget =
           resolver.Resolve(libgui, descriptor.apiSignature);
       if (nullptr == *descriptor.storeToTarget && !descriptor.optional) {
         LogError(
             "[!] Version[Android %zu] [libgui] failed to resolve symbol: %s",
             compat::SystemVersion, descriptor.apiSignature);
         throw std::runtime_error(
             std::string("Failed to resolve [libgui] symbol: ") +
             descriptor.apiSignature);
       }
     }
 
     resolver.Close(libutils);
     resolver.Close(libgui);
     resolved = true;
     LogInfo("[+] Version[Android %zu] resolved all symbols",
             compat::SystemVersion);
   }
 };
 
 struct DumpDisplayInfo {
   std::string uniqueId;
   uint32_t currentLayerStack;
   struct {
     int32_t left;
     int32_t top;
     int32_t right;
     int32_t bottom;
   } currentLayerStackRect;
 
   static DumpDisplayInfo MakeFromRawDumpInfo(
       const std::string_view& uniqueId,
       const std::string_view& currentLayerStack,
       const std::string_view& currentLayerStackRect) {
     // Zero-allocation integer helpers — from_chars operates directly on
     // the string_view data without constructing temporary std::strings.
     const auto ParseI32 = [](std::string_view sv) -> int32_t {
       while (!sv.empty() && sv[0] == ' ') sv.remove_prefix(1);
       int32_t v = 0;
       std::from_chars(sv.data(), sv.data() + sv.size(), v);
       return v;
     };
     const auto ParseU32 = [](std::string_view sv) -> uint32_t {
       while (!sv.empty() && sv[0] == ' ') sv.remove_prefix(1);
       uint32_t v = 0;
       std::from_chars(sv.data(), sv.data() + sv.size(), v);
       return v;
     };
 
     DumpDisplayInfo result;
 
     result.uniqueId = std::string{uniqueId};  // one unavoidable allocation
     result.currentLayerStack = ParseU32(currentLayerStack);
 
     const auto leftPos = currentLayerStackRect.find('(') + 1;
     const auto topPos = currentLayerStackRect.find(", ", leftPos);
     const auto rightPos = currentLayerStackRect.find(" - ", topPos + 2);
     const auto bottomPos = currentLayerStackRect.find(", ", rightPos + 3);
     const auto endPos = currentLayerStackRect.find(')', bottomPos + 2);
 
     result.currentLayerStackRect.left =
         ParseI32(currentLayerStackRect.substr(leftPos, topPos - leftPos));
     result.currentLayerStackRect.top = ParseI32(
         currentLayerStackRect.substr(topPos + 2, rightPos - topPos - 2));
     result.currentLayerStackRect.right = ParseI32(
         currentLayerStackRect.substr(rightPos + 3, bottomPos - rightPos - 3));
     result.currentLayerStackRect.bottom = ParseI32(
         currentLayerStackRect.substr(bottomPos + 2, endPos - bottomPos - 2));
 
     return result;
   }
 };
 
 struct MirrorLayerTransform {
   // True when source and target have opposite orientations
   // (landscape↔portrait), meaning a 90° CCW rotation is applied in addition to
   // scale.
   bool needs_rotation;
   // 2×2 matrix in AOSP setMatrix native order: dsdx, dtdx, dtdy, dsdy.
   // Pass these positionally to SurfaceComposerClientTransaction::SetMatrix.
   float dsdx, dtdx, dtdy, dsdy;
   float offsetX, offsetY;
 };
 
 // Single-pass O(n) line scanner — avoids re-scanning the full dump string
 // for every "DisplayDeviceInfo" entry the old multi-find approach caused.
 inline std::vector<DumpDisplayInfo> ParseDumpDisplayInfo(
     const std::string_view& dumpDisplayInfo) {
   std::vector<DumpDisplayInfo> result;
 
   bool in_block = false;
   std::string_view unique_id, layer_stack, layer_stack_rect;
   bool has_id = false, has_stack = false, has_rect = false;
 
   auto FlushBlock = [&] {
     if (!in_block || !has_id || !has_stack || !has_rect) return;
     if (layer_stack == "-1") {
       LogError("[-] %s -> Current layer stack is -1, skipping",
                std::string{unique_id.begin(), unique_id.end()}.data());
     } else {
       result.push_back(DumpDisplayInfo::MakeFromRawDumpInfo(
           unique_id, layer_stack, layer_stack_rect));
     }
     in_block = has_id = has_stack = has_rect = false;
   };
 
   size_t pos = 0;
   while (pos < dumpDisplayInfo.size()) {
     const size_t eol =
         std::min(dumpDisplayInfo.find('\n', pos), dumpDisplayInfo.size());
     const std::string_view line = dumpDisplayInfo.substr(pos, eol - pos);
     pos = eol + 1;
 
     size_t vpos;
     if (line.find("DisplayDeviceInfo") != std::string_view::npos) {
       FlushBlock();
       in_block = true;
     } else if (in_block) {
       // Check rect before stack — "mCurrentLayerStackRect" contains
       // "mCurrentLayerStack" as a prefix so ordering prevents false match.
       if ((vpos = line.find("mCurrentLayerStackRect=")) !=
           std::string_view::npos) {
         layer_stack_rect =
             line.substr(vpos + sizeof("mCurrentLayerStackRect=") - 1);
         has_rect = true;
       } else if ((vpos = line.find("mCurrentLayerStack=")) !=
                  std::string_view::npos) {
         layer_stack = line.substr(vpos + sizeof("mCurrentLayerStack=") - 1);
         has_stack = true;
       } else if ((vpos = line.find("mUniqueId=")) != std::string_view::npos) {
         unique_id = line.substr(vpos + sizeof("mUniqueId=") - 1);
         has_id = true;
       }
     }
   }
   FlushBlock();
 
   return result;
 }
 
 inline MirrorLayerTransform CalcMirrorLayerTransform(float targetWidth,
                                                      float targetHeight,
                                                      float sourceWidth,
                                                      float sourceHeight) {
   if (0.f == targetHeight || 0.f == sourceHeight) {
     (::fprintf(stderr, "[native_window] %s\n", "[-] Invalid height"),
      ::abort());
   }
 
   const bool src_landscape = sourceWidth >= sourceHeight;
   const bool tgt_landscape = targetWidth >= targetHeight;
 
   if (src_landscape != tgt_landscape) {
     // Orientations differ: apply 90° CCW rotation so landscape content
     // fills a portrait frame (or vice versa).
     // 90° CCW maps source (x,y) → target (-s·y + tx, s·x + ty).
     // AOSP setMatrix native parameters: dsdx=0, dtdx=-s, dtdy=s, dsdy=0.
     const float scale_x = targetWidth / sourceHeight;
     const float scale_y = targetHeight / sourceWidth;
     const float scale = std::min(scale_x, scale_y);
     const float used_w = sourceHeight * scale;
     const float used_h = sourceWidth * scale;
     const float margin_x = (targetWidth - used_w) * 0.5f;
     const float margin_y = (targetHeight - used_h) * 0.5f;
     return MirrorLayerTransform{
         .needs_rotation = true,
         .dsdx = 0.f,
         .dtdx = -scale,
         .dtdy = scale,
         .dsdy = 0.f,
         // After 90° CCW the content extends to negative x; shift right by
         // usedW to bring it back into the frame, then add the centering margin.
         .offsetX = margin_x + used_w,
         .offsetY = margin_y,
     };
   }
 
   // Same orientation — pure scale with letterbox/pillarbox centering.
   const float scale_x = targetWidth / sourceWidth;
   const float scale_y = targetHeight / sourceHeight;
   const float scale = std::min(scale_x, scale_y);
   const float used_w = sourceWidth * scale;
   const float used_h = sourceHeight * scale;
   return MirrorLayerTransform{
       .needs_rotation = false,
       .dsdx = scale,
       .dtdx = 0.f,
       .dtdy = 0.f,
       .dsdy = scale,
       .offsetX = (targetWidth - used_w) * 0.5f,
       .offsetY = (targetHeight - used_h) * 0.5f,
   };
 }
 }  // namespace android::anative_window_creator::detail
 
 namespace android {
 class ANativeWindowCreator {
  public:
   struct DisplayInfo {
     int32_t theta;
     int32_t width;
     int32_t height;
   };
 
   struct CreateOptions {
     const char* name;
     int32_t width;
     int32_t height;
     bool skipScreenshot;
   };
 
  public:
   static void SetupCustomApiResolver(
       const anative_window_creator::detail::ApiResolver::ResolverImpl&
           resolver) {
     anative_window_creator::detail::ApiResolver::Resolve(resolver);
   }
 
  public:
   static anative_window_creator::detail::compat::SurfaceComposerClient&
   GetComposerInstance() {
     anative_window_creator::detail::ApiResolver::Resolve();
 
     static anative_window_creator::detail::compat::SurfaceComposerClient
         surfaceComposerClient;
 
     return surfaceComposerClient;
   }
 
   static DisplayInfo GetDisplayInfo() {
     auto& surfaceComposerClient = GetComposerInstance();
     anative_window_creator::detail::types::ui::DisplayState displayInfo{};
 
     if (!surfaceComposerClient.GetDisplayInfo(&displayInfo)) {
       return {};
     }
 
     return DisplayInfo{
         .theta = 90 * static_cast<int32_t>(displayInfo.orientation),
         .width = displayInfo.layerStackSpaceRect.width,
         .height = displayInfo.layerStackSpaceRect.height,
     };
   }
 
   // Body in src/secure_capture.cpp (strong symbol; OR prevent_capture flag)
  static ANativeWindow* Create(const CreateOptions& options = {
                                   .name = "AImGui",
                                   .width = 0,
                                   .height = 0,
                                   .skipScreenshot = false});

  static void Destroy(ANativeWindow* nativeWindow) {
     if (!m_cachedSurfaceControl.contains(nativeWindow)) {
       return;
     }
 
     m_cachedSurfaceControl[nativeWindow].DestroySurface(
         reinterpret_cast<anative_window_creator::detail::compat::Surface*>(
             nativeWindow));
     m_cachedSurfaceControl.erase(nativeWindow);
   }
 
   // renderScalePct is the overlay's live render-resolution percent (10..100).
   // A SurfaceFlinger mirror clones the SOURCE LAYER'S BUFFER, not its
   // upscaled on-screen presentation, so when the overlay renders at a reduced
   // buffer the clone's intrinsic content is that smaller buffer. The transform
   // must treat the source as builtinDisplay*pct/100, or the clone is
   // under-scaled and lands in the top-left fraction of the capture frame
   // instead of filling it edge to edge.
   static void ProcessMirrorDisplay(int32_t renderScalePct = 100) {
     renderScalePct = std::clamp<int32_t>(renderScalePct, 10, 100);
     static std::chrono::steady_clock::time_point lastTime{};
 
     if (14 > anative_window_creator::detail::compat::SystemVersion) {
       return;
     }
     if (std::chrono::steady_clock::now() - lastTime <
         std::chrono::milliseconds(500)) {
       return;
     }
 
     // Check if have surfaces need to mirror
     static std::vector<anative_window_creator::detail::compat::SurfaceControl*>
         surfacesNeedToMirror;
 
     surfacesNeedToMirror.clear();
     for (auto& [_, surfaceControl] : m_cachedSurfaceControl) {
       if (!surfaceControl.skipScreenshot) {
         surfacesNeedToMirror.push_back(&surfaceControl);
       }
     }
     if (surfacesNeedToMirror.empty()) {
       return;
     }
 
     // Run "dumpsys display" and get result
     auto pipe = popen("dumpsys display", "r");
     if (!pipe) {
       LogError("[-] Failed to run dumpsys command");
       return;
     }
 
     char buffer[512]{};
     std::string dumpDisplayResult;
     dumpDisplayResult.reserve(65536);
     while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
       dumpDisplayResult += buffer;
     }
     pclose(pipe);
 
     static std::unordered_map<
         uint32_t,
         std::vector<anative_window_creator::detail::compat::SurfaceControl>>
         cachedLayerStackMirrorSurfaces;
     static std::unordered_set<uint32_t> cachedLayerStackScales;
 
     auto dumpDisplayInfos =
         anative_window_creator::detail::ParseDumpDisplayInfo(dumpDisplayResult);
 
     // Evict stale entries for virtual displays that have disconnected.
     // Linear any_of over the tiny display list (N≤4) avoids the
     // unordered_set bucket-array heap allocation the set-based version
     // triggered every 500 ms.  Erasing from s_mirror_handles calls
     // DeleteMirrorPair, which disconnects and releases the native surfaces.
     for (auto it = cachedLayerStackMirrorSurfaces.begin();
          it != cachedLayerStackMirrorSurfaces.end();) {
       const bool still_active = std::any_of(
           dumpDisplayInfos.begin(), dumpDisplayInfos.end(),
           [&](const auto& d) { return d.currentLayerStack == it->first; });
       if (!still_active) {
         for (auto& sc : it->second) {
           GetComposerInstance().s_mirror_handles.erase(sc.data);
         }
         cachedLayerStackScales.erase(it->first);
         it = cachedLayerStackMirrorSurfaces.erase(it);
       } else {
         ++it;
       }
     }
 
     // Pass 1: capture builtin display dims before processing virtual stacks.
     // dumpsys output order is not guaranteed — the virtual display entry can
     // appear before stack-0, so doing this in the same loop as the transform
     // step would leave builtinDisplayWidth==-1 and silently skip the scale.
     static int32_t builtinDisplayWidth = -1, builtinDisplayHeight = -1;
     for (const auto& d : dumpDisplayInfos) {
       if (d.currentLayerStack == 0) {
         builtinDisplayWidth = d.currentLayerStackRect.right;
         builtinDisplayHeight = d.currentLayerStackRect.bottom;
         break;
       }
     }
 
     // A runtime render-scale change resizes the source buffer, so every
     // cached transform is now sized for the wrong resolution. Drop the
     // applied-scale cache so Pass 2 recomputes each mirror's matrix against
     // the new source size. The mirror SURFACES themselves are untouched —
     // only the scale/position matrix is re-applied (cheap, no reallocation).
     static int32_t lastAppliedRenderScalePct = -1;
     if (renderScalePct != lastAppliedRenderScalePct) {
       cachedLayerStackScales.clear();
       lastAppliedRenderScalePct = renderScalePct;
     }
 
     // Pass 2: mirror and transform every virtual display stack (stack != 0).
     for (auto& displayInfo : dumpDisplayInfos) {
       if (0 == displayInfo.currentLayerStack) {
         continue;
       }
 
       if (!cachedLayerStackMirrorSurfaces.contains(
               displayInfo.currentLayerStack)) {
         LogInfo("[=] New display layerstack detected: [%s] -> %u",
                 displayInfo.uniqueId.data(), displayInfo.currentLayerStack);
 
         for (auto surfaceControl : surfacesNeedToMirror) {
           auto mirrorLayer = GetComposerInstance().MirrorSurface(
               *surfaceControl, displayInfo.currentLayerStack,
               displayInfo.currentLayerStackRect.right,
               displayInfo.currentLayerStackRect.bottom);
 
           LogInfo("[+] Mirror layer created: %p", mirrorLayer.data);
           cachedLayerStackMirrorSurfaces[displayInfo.currentLayerStack]
               .emplace_back(std::move(mirrorLayer));
         }
       }
 
       if (-1 != builtinDisplayWidth && -1 != builtinDisplayHeight &&
           cachedLayerStackMirrorSurfaces.contains(
               displayInfo.currentLayerStack)) {
         if (!cachedLayerStackScales.contains(displayInfo.currentLayerStack)) {
           // Forced to logcat in release too (see note at the transform
           // print below): shows source builtin dims -> target virtual rect.
           __android_log_print(
               ANDROID_LOG_INFO, LOGTAG,
               "[=] Display layerstack transform[%d x %d @ %d%%]: %u -> %d x %d",
               builtinDisplayWidth, builtinDisplayHeight, renderScalePct,
               displayInfo.currentLayerStack,
               displayInfo.currentLayerStackRect.right,
               displayInfo.currentLayerStackRect.bottom);
 
           auto& composerInstance = GetComposerInstance();
           auto& mirrorLayers =
               cachedLayerStackMirrorSurfaces.at(displayInfo.currentLayerStack);
           // Build the scale+rotation transform. CalcMirrorLayerTransform
           // takes (target, source): the virtual display rect is the "target"
           // frame the clone is scaled into. The "source" is the clone's
           // intrinsic content size — the overlay's render BUFFER, which is
           // builtinDisplay*renderScalePct/100 (the clone reflects the buffer,
           // not the full-screen-upscaled layer). Using the full builtin size
           // here would under-scale the clone whenever the overlay renders
           // below 100%, leaving it in the top-left fraction of the frame.
           // The transform is applied to the clone itself (see MirrorSurface),
           // not the root container.
           const float renderScale = static_cast<float>(renderScalePct) / 100.f;
           const auto t =
               anative_window_creator::detail::CalcMirrorLayerTransform(
                   static_cast<float>(displayInfo.currentLayerStackRect.right),
                   static_cast<float>(displayInfo.currentLayerStackRect.bottom),
                   static_cast<float>(builtinDisplayWidth) * renderScale,
                   static_cast<float>(builtinDisplayHeight) * renderScale);
 
           for (auto& mirrorLayer : mirrorLayers) {
             composerInstance.TransformSurface(mirrorLayer, t.dsdx, t.dtdx,
                                               t.dtdy, t.dsdy, t.offsetX,
                                               t.offsetY);
             // Force this one line to logcat even in release builds, where
             // LogInfo is stubbed to a no-op by native_window.cpp. Bypasses
             // the macro with a direct call so no other AImGui logs return.
             __android_log_print(ANDROID_LOG_INFO, LOGTAG,
                                 "[+] Transform mirror layer:%p rot=%d "
                                 "matrix=[%.3f,%.3f,%.3f,%.3f] pos=(%.1f,%.1f)",
                                 mirrorLayer.data, t.needs_rotation, t.dsdx,
                                 t.dtdx, t.dtdy, t.dsdy, t.offsetX, t.offsetY);
           }
           cachedLayerStackScales.emplace(displayInfo.currentLayerStack);
         }
       }
     }
 
     lastTime = std::chrono::steady_clock::now();
   }
 
   static void UpdateWindowInfo(ANativeWindow* nativeWindow, void* windowInfo) {
     if (!m_cachedSurfaceControl.contains(nativeWindow)) {
       return;
     }
 
     auto& transaction = GetComposerInstance().GetDefaultTransaction();
     auto& surfaceControl = m_cachedSurfaceControl.at(nativeWindow);
     auto parentingLayer = surfaceControl.GetParentingLayer();
 
     transaction.SetInputWindowInfo(parentingLayer, windowInfo);
     transaction.Apply(true, false);
   }
 
  private:
   inline static std::unordered_map<
       ANativeWindow*, anative_window_creator::detail::compat::SurfaceControl>
       m_cachedSurfaceControl;
 };
 }  // namespace android
 
 #if LOGTAG_DEFINED
 #undef LOGTAG
 
 #undef LOGTAG_DEFINED
 #endif  // LOGTAG_DEFINED
 
 #if LogInfo_DEFINED
 #undef LogInfo
 #undef LogDebug
 #undef LogError
 
 #undef LogInfo_DEFINED
 #endif  // LogInfo_DEFINED
 
 #endif  // !A_NATIVE_WINDOW_CREATOR_H