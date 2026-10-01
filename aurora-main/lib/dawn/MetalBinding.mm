#include "BackendBinding.hpp"

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

// macOS composites a window's CAMetalLayer at 60 Hz on ProMotion displays unless something in the
// window asks for more; with V-Sync off that shows up as nextDrawable blocking the producer at 60 fps.
// A display link on the view carrying the display's full rate range is that request. Its callback
// does nothing: the guest still paces itself.
@interface AuroraFrameRateHint : NSObject
- (void)tick:(id)link;
@end
@implementation AuroraFrameRateHint
- (void)tick:(id)link {
}
@end
#endif
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

namespace aurora::webgpu::utils {
namespace {
constexpr const char* MetalViewProperty = "aurora.window.metal_view";

void SDLCALL DestroyMetalView(void*, void* value) {
  SDL_Metal_DestroyView(value);
}
} // namespace

std::shared_ptr<wgpu::ChainedStruct> SetupWindowAndGetSurfaceDescriptorCocoa(SDL_Window* window) {
  const auto properties = SDL_GetWindowProperties(window);
  if (!properties) {
    return nullptr;
  }
  auto view = SDL_GetPointerProperty(properties, MetalViewProperty, nullptr);
  if (!view) {
    view = SDL_Metal_CreateView(window);
    if (!view) {
      return nullptr;
    }
    // Own one view per window, not per WebGPU surface. Surface recovery must
    // preserve the UIKit root and its controls (and the Cocoa Metal subview).
    // SDL cleans window properties before destroying its native window.
    // The cleanup callback also runs if setting the property fails.
    if (!SDL_SetPointerPropertyWithCleanup(properties, MetalViewProperty, view, DestroyMetalView, nullptr)) {
      return nullptr;
    }
  }
#if TARGET_OS_OSX
  if (@available(macOS 14.0, *)) {
    NSView* nsView = (__bridge NSView*)view;
    static char kHintKey;
    if (nsView && !objc_getAssociatedObject(nsView, &kHintKey)) {
      AuroraFrameRateHint* hint = [AuroraFrameRateHint new];
      CADisplayLink* link = [nsView displayLinkWithTarget:hint selector:@selector(tick:)];
      const float maxRate = 240.0f;
      link.preferredFrameRateRange = CAFrameRateRangeMake(60.0f, maxRate, maxRate);
      [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
      objc_setAssociatedObject(nsView, &kHintKey, link, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
  }
#endif
  auto desc = std::make_shared<wgpu::SurfaceSourceMetalLayer>();
  desc->layer = SDL_Metal_GetLayer(view);
  if (!desc->layer) {
    SDL_ClearProperty(properties, MetalViewProperty);
    return nullptr;
  }
  return desc;
}
} // namespace aurora::webgpu::utils
