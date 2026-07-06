#include <mbgl/gl/headless_backend.hpp>
#include <mbgl/util/logging.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Foundation/NSString.h>
#include <OpenGL/OpenGL.h>

#include <stdexcept>
#include <string>

namespace mbgl {
namespace gl {

namespace {

bool choosePixelFormat(const CGLPixelFormatAttribute* attributes, CGLPixelFormatObj& out) {
    GLint num = 0;
    const CGLError error = CGLChoosePixelFormat(attributes, &out, &num);
    return error == kCGLNoError && num > 0 && out != nullptr;
}

} // namespace

// This class provides a singleton that contains information about the pixel format used for
// instantiating new headless rendering contexts.
class CGLDisplayConfig {
private:
  // Key for singleton construction.
  struct Key {
    explicit Key() = default;
  };

public:
  CGLDisplayConfig(Key) {
    // mbgl requires GL 3.x+ (VAO, UBO, FBO). Legacy profile causes GL_INVALID_* errors.
    const CGLPixelFormatAttribute attrs41[] = {
        kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
        kCGLPFAClosestPolicy,
        kCGLPFAColorSize, static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAAlphaSize, static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFADepthSize, static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAStencilSize, static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFAAllowOfflineRenderers,
        static_cast<CGLPixelFormatAttribute>(0),
    };

    const CGLPixelFormatAttribute attrs32[] = {
        kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
        kCGLPFAClosestPolicy,
        kCGLPFAColorSize, static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAAlphaSize, static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFADepthSize, static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAStencilSize, static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFAAllowOfflineRenderers,
        static_cast<CGLPixelFormatAttribute>(0),
    };

    if (!choosePixelFormat(attrs41, pixelFormat) && !choosePixelFormat(attrs32, pixelFormat)) {
      throw std::runtime_error("No suitable OpenGL Core pixel format found for headless rendering.");
    }
  }

  ~CGLDisplayConfig() {
    const CGLError error = CGLDestroyPixelFormat(pixelFormat);
    if (error != kCGLNoError) {
      Log::Error(Event::OpenGL,
                 std::string("Error destroying pixel format:") + CGLErrorString(error));
    }
  }

  static std::shared_ptr<const CGLDisplayConfig> create() {
    static std::weak_ptr<const CGLDisplayConfig> instance;
    auto shared = instance.lock();
    if (!shared) {
      instance = shared = std::make_shared<CGLDisplayConfig>(Key{});
    }
    return shared;
  }

public:
  CGLPixelFormatObj pixelFormat = nullptr;
};

class CGLBackendImpl final : public HeadlessBackend::Impl {
public:
  CGLBackendImpl() {
    CGLError error = CGLCreateContext(cglDisplay->pixelFormat, nullptr, &glContext);
    if (error != kCGLNoError) {
      throw std::runtime_error(std::string("Error creating GL context object:") +
                               CGLErrorString(error) + "\n");
    }

    // kCGLCEMPEngine is deprecated and may fail on Core profile contexts; non-fatal.
    CGLEnable(glContext, kCGLCEMPEngine);
  }

  ~CGLBackendImpl() final { CGLDestroyContext(glContext); }

  gl::ProcAddress getExtensionFunctionPointer(const char* name) final {
    static CFBundleRef framework = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.opengl"));
    if (!framework) {
      throw std::runtime_error("Failed to load OpenGL framework.");
    }

    return reinterpret_cast<gl::ProcAddress>(CFBundleGetFunctionPointerForName(
        framework, (__bridge CFStringRef)[NSString stringWithUTF8String:name]));
  }

  void activateContext() final {
    CGLError error = CGLSetCurrentContext(glContext);
    if (error != kCGLNoError) {
      throw std::runtime_error(std::string("Switching OpenGL context failed:") +
                               CGLErrorString(error) + "\n");
    }
  }

  void deactivateContext() final {
    CGLError error = CGLSetCurrentContext(nullptr);
    if (error != kCGLNoError) {
      throw std::runtime_error(std::string("Removing OpenGL context failed:") +
                               CGLErrorString(error) + "\n");
    }
  }

private:
  const std::shared_ptr<const CGLDisplayConfig> cglDisplay = CGLDisplayConfig::create();
  CGLContextObj glContext = nullptr;
};

void HeadlessBackend::createImpl() {
  assert(!impl);
  impl = std::make_unique<CGLBackendImpl>();
}

}  // namespace gl
}  // namespace mbgl
