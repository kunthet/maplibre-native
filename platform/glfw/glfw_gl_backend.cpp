#include "glfw_gl_backend.hpp"

#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/gl/renderable_resource.hpp>
#include <mbgl/platform/gl_functions.hpp>
#include <mbgl/util/instrumentation.hpp>

#include <GLFW/glfw3.h>

#include <array>
#include <string>

namespace {

GLuint compileShader(mbgl::platform::GLenum type, const char* source) {
    const mbgl::platform::GLuint shader = MBGL_CHECK_ERROR(mbgl::platform::glCreateShader(type));
    MBGL_CHECK_ERROR(mbgl::platform::glShaderSource(shader, 1, &source, nullptr));
    MBGL_CHECK_ERROR(mbgl::platform::glCompileShader(shader));
    GLint compiled = GL_FALSE;
    MBGL_CHECK_ERROR(mbgl::platform::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled));
    if (!compiled) {
        GLint length = 0;
        MBGL_CHECK_ERROR(mbgl::platform::glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length));
        std::string log(static_cast<size_t>(length > 1 ? length : 1), '\0');
        MBGL_CHECK_ERROR(mbgl::platform::glGetShaderInfoLog(shader, length, nullptr, log.data()));
        MBGL_CHECK_ERROR(mbgl::platform::glDeleteShader(shader));
        return 0;
    }
    return shader;
}

GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader) {
    const GLuint program = MBGL_CHECK_ERROR(mbgl::platform::glCreateProgram());
    MBGL_CHECK_ERROR(mbgl::platform::glAttachShader(program, vertexShader));
    MBGL_CHECK_ERROR(mbgl::platform::glAttachShader(program, fragmentShader));
    MBGL_CHECK_ERROR(mbgl::platform::glLinkProgram(program));
    GLint linked = GL_FALSE;
    MBGL_CHECK_ERROR(mbgl::platform::glGetProgramiv(program, GL_LINK_STATUS, &linked));
    if (!linked) {
        MBGL_CHECK_ERROR(mbgl::platform::glDeleteProgram(program));
        return 0;
    }
    return program;
}

} // namespace

struct GLFWGLBackend::PanSnapshot {
    mbgl::Size size{0, 0};
    GLuint texture = 0;
    GLuint program = 0;
    GLuint vbo = 0;
    GLint sizeLocation = -1;
    GLint offsetLocation = -1;
    GLint texLocation = -1;

    ~PanSnapshot() { destroy(); }

    void destroy() {
        if (texture) {
            mbgl::platform::glDeleteTextures(1, &texture);
            texture = 0;
        }
        if (program) {
            mbgl::platform::glDeleteProgram(program);
            program = 0;
        }
        if (vbo) {
            mbgl::platform::glDeleteBuffers(1, &vbo);
            vbo = 0;
        }
        size = {0, 0};
        sizeLocation = -1;
        offsetLocation = -1;
        texLocation = -1;
    }

    void clearSnapshot() {
        if (texture) {
            mbgl::platform::glDeleteTextures(1, &texture);
            texture = 0;
        }
        size = {0, 0};
    }

    bool ensureProgram() {
        if (program) {
            return true;
        }

        static const char* vertexSource = R"(attribute vec2 a_pos;
attribute vec2 a_uv;
uniform vec2 u_size;
uniform vec2 u_offset;
varying vec2 v_uv;
void main() {
    vec2 pixel = a_pos + u_offset;
    gl_Position = vec4(pixel.x / u_size.x * 2.0 - 1.0, pixel.y / u_size.y * 2.0 - 1.0, 0.0, 1.0);
    v_uv = a_uv;
})";

        static const char* fragmentSource = R"(precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
void main() {
    gl_FragColor = texture2D(u_tex, v_uv);
})";

        const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
        if (!vertexShader) {
            return false;
        }
        const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (!fragmentShader) {
            MBGL_CHECK_ERROR(mbgl::platform::glDeleteShader(vertexShader));
            return false;
        }
        program = linkProgram(vertexShader, fragmentShader);
        MBGL_CHECK_ERROR(mbgl::platform::glDeleteShader(vertexShader));
        MBGL_CHECK_ERROR(mbgl::platform::glDeleteShader(fragmentShader));
        if (!program) {
            return false;
        }

        sizeLocation = MBGL_CHECK_ERROR(mbgl::platform::glGetUniformLocation(program, "u_size"));
        offsetLocation = MBGL_CHECK_ERROR(mbgl::platform::glGetUniformLocation(program, "u_offset"));
        texLocation = MBGL_CHECK_ERROR(mbgl::platform::glGetUniformLocation(program, "u_tex"));

        MBGL_CHECK_ERROR(mbgl::platform::glGenBuffers(1, &vbo));
        return sizeLocation >= 0 && offsetLocation >= 0 && texLocation >= 0;
    }

    bool prepareTexture(const mbgl::Size& captureSize) {
        if (captureSize.width == 0 || captureSize.height == 0 || !ensureProgram()) {
            return false;
        }

        if (!texture) {
            MBGL_CHECK_ERROR(mbgl::platform::glGenTextures(1, &texture));
        }

        MBGL_CHECK_ERROR(mbgl::platform::glBindTexture(GL_TEXTURE_2D, texture));
        MBGL_CHECK_ERROR(mbgl::platform::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        MBGL_CHECK_ERROR(mbgl::platform::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        MBGL_CHECK_ERROR(mbgl::platform::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        MBGL_CHECK_ERROR(mbgl::platform::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        MBGL_CHECK_ERROR(mbgl::platform::glTexImage2D(GL_TEXTURE_2D,
                                      0,
                                      GL_RGBA,
                                      static_cast<GLsizei>(captureSize.width),
                                      static_cast<GLsizei>(captureSize.height),
                                      0,
                                      GL_RGBA,
                                      GL_UNSIGNED_BYTE,
                                      nullptr));
        return true;
    }

    bool captureFromBackBuffer(const mbgl::Size& captureSize) {
        if (!prepareTexture(captureSize)) {
            return false;
        }

        MBGL_CHECK_ERROR(mbgl::platform::glFinish());
        // MapLibre just rendered into the back buffer; copy before swap.
        MBGL_CHECK_ERROR(mbgl::platform::glCopyTexSubImage2D(GL_TEXTURE_2D,
                                             0,
                                             0,
                                             0,
                                             0,
                                             0,
                                             static_cast<GLsizei>(captureSize.width),
                                             static_cast<GLsizei>(captureSize.height)));
        MBGL_CHECK_ERROR(mbgl::platform::glBindTexture(GL_TEXTURE_2D, 0));

        size = captureSize;
        return true;
    }

    bool captureFromDisplayedBuffer(const mbgl::Size& captureSize) {
        if (!prepareTexture(captureSize)) {
            return false;
        }

        MBGL_CHECK_ERROR(mbgl::platform::glFinish());
        MBGL_CHECK_ERROR(mbgl::platform::glReadBuffer(GL_FRONT));
        MBGL_CHECK_ERROR(mbgl::platform::glCopyTexSubImage2D(GL_TEXTURE_2D,
                                             0,
                                             0,
                                             0,
                                             0,
                                             0,
                                             static_cast<GLsizei>(captureSize.width),
                                             static_cast<GLsizei>(captureSize.height)));
        MBGL_CHECK_ERROR(mbgl::platform::glReadBuffer(GL_BACK));
        MBGL_CHECK_ERROR(mbgl::platform::glBindTexture(GL_TEXTURE_2D, 0));

        size = captureSize;
        return true;
    }

    void draw(const mbgl::Size& drawSize, float offsetX, float offsetY) {
        if (!texture || size.width == 0 || size.height == 0 || !ensureProgram()) {
            return;
        }

        MBGL_CHECK_ERROR(mbgl::platform::glBindVertexArray(0));
        MBGL_CHECK_ERROR(mbgl::platform::glDisable(GL_DEPTH_TEST));
        MBGL_CHECK_ERROR(mbgl::platform::glDisable(GL_STENCIL_TEST));
        MBGL_CHECK_ERROR(mbgl::platform::glDisable(GL_SCISSOR_TEST));
        MBGL_CHECK_ERROR(mbgl::platform::glDisable(GL_BLEND));
        MBGL_CHECK_ERROR(mbgl::platform::glDisable(GL_CULL_FACE));
        MBGL_CHECK_ERROR(mbgl::platform::glViewport(0, 0, static_cast<GLsizei>(drawSize.width), static_cast<GLsizei>(drawSize.height)));
        MBGL_CHECK_ERROR(mbgl::platform::glClearColor(1.f, 1.f, 1.f, 1.f));
        MBGL_CHECK_ERROR(mbgl::platform::glClear(GL_COLOR_BUFFER_BIT));

        MBGL_CHECK_ERROR(mbgl::platform::glUseProgram(program));
        MBGL_CHECK_ERROR(mbgl::platform::glUniform2f(sizeLocation,
                                     static_cast<GLfloat>(size.width),
                                     static_cast<GLfloat>(size.height)));
        MBGL_CHECK_ERROR(mbgl::platform::glUniform2f(offsetLocation, offsetX, offsetY));

        MBGL_CHECK_ERROR(mbgl::platform::glActiveTexture(GL_TEXTURE0));
        MBGL_CHECK_ERROR(mbgl::platform::glBindTexture(GL_TEXTURE_2D, texture));
        MBGL_CHECK_ERROR(mbgl::platform::glUniform1i(texLocation, 0));

        MBGL_CHECK_ERROR(mbgl::platform::glBindBuffer(GL_ARRAY_BUFFER, vbo));
        const GLint posLocation = MBGL_CHECK_ERROR(mbgl::platform::glGetAttribLocation(program, "a_pos"));
        const GLint uvLocation = MBGL_CHECK_ERROR(mbgl::platform::glGetAttribLocation(program, "a_uv"));
        if (posLocation < 0 || uvLocation < 0) {
            return;
        }
        MBGL_CHECK_ERROR(mbgl::platform::glEnableVertexAttribArray(static_cast<GLuint>(posLocation)));
        MBGL_CHECK_ERROR(mbgl::platform::glEnableVertexAttribArray(static_cast<GLuint>(uvLocation)));
        MBGL_CHECK_ERROR(mbgl::platform::glVertexAttribPointer(static_cast<GLuint>(posLocation), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(0)));
        MBGL_CHECK_ERROR(mbgl::platform::glVertexAttribPointer(static_cast<GLuint>(uvLocation), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(2 * sizeof(GLfloat))));

        const float w = static_cast<float>(size.width);
        const float h = static_cast<float>(size.height);
        const std::array<GLfloat, 24> vertices = {
            0.f, 0.f, 0.f, 0.f, //
            w,   0.f, 1.f, 0.f, //
            0.f, h,   0.f, 1.f, //
            w,   0.f, 1.f, 0.f, //
            w,   h,   1.f, 1.f, //
            0.f, h,   0.f, 1.f, //
        };
        MBGL_CHECK_ERROR(mbgl::platform::glBufferData(GL_ARRAY_BUFFER, GLsizei(sizeof(vertices)), vertices.data(), GL_DYNAMIC_DRAW));

        MBGL_CHECK_ERROR(mbgl::platform::glDrawArrays(GL_TRIANGLES, 0, 6));

        MBGL_CHECK_ERROR(mbgl::platform::glDisableVertexAttribArray(static_cast<GLuint>(posLocation)));
        MBGL_CHECK_ERROR(mbgl::platform::glDisableVertexAttribArray(static_cast<GLuint>(uvLocation)));
        MBGL_CHECK_ERROR(mbgl::platform::glBindBuffer(GL_ARRAY_BUFFER, 0));
        MBGL_CHECK_ERROR(mbgl::platform::glBindTexture(GL_TEXTURE_2D, 0));
        MBGL_CHECK_ERROR(mbgl::platform::glUseProgram(0));
    }
};

class GLFWGLRenderableResource final : public mbgl::gl::RenderableResource {
public:
    explicit GLFWGLRenderableResource(GLFWGLBackend& backend_)
        : backend(backend_) {}

    void bind() override {
        MLN_TRACE_FUNC();

        backend.setFramebufferBinding(0);
        backend.setViewport(0, 0, backend.getSize());
    }

    void swap() override {
        MLN_TRACE_FUNC();

        backend.swap();
    }

private:
    GLFWGLBackend& backend;
};

GLFWGLBackend::GLFWGLBackend(GLFWwindow* window_, const bool capFrameRate)
    : mbgl::gl::RendererBackend(mbgl::gfx::ContextMode::Unique),
      mbgl::gfx::Renderable(
          [window_] {
              int fbWidth;
              int fbHeight;
              glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
              return mbgl::Size{static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight)};
          }(),
          std::make_unique<GLFWGLRenderableResource>(*this)),
      window(window_),
      vsyncEnabled(capFrameRate) {
    MLN_TRACE_FUNC();

    glfwMakeContextCurrent(window);
    if (!capFrameRate) {
        // Disables vsync on platforms that support it.
        glfwSwapInterval(0);
        vsyncEnabled = false;
    } else {
        glfwSwapInterval(1);
        vsyncEnabled = true;
    }
}

GLFWGLBackend::~GLFWGLBackend() {
    activate();
    if (panSnapshot) {
        panSnapshot->destroy();
        panSnapshot.reset();
    }
}

void GLFWGLBackend::resetRendererStateAfterExternalDraw() {
    getContext().setDirtyState();
    updateAssumedState();
}

void GLFWGLBackend::activate() {
    MLN_TRACE_FUNC();

    glfwMakeContextCurrent(window);
}

void GLFWGLBackend::deactivate() {
    MLN_TRACE_FUNC();

    glfwMakeContextCurrent(nullptr);
}

mbgl::gl::ProcAddress GLFWGLBackend::getExtensionFunctionPointer(const char* name) {
    return glfwGetProcAddress(name);
}

void GLFWGLBackend::updateAssumedState() {
    MLN_TRACE_FUNC();

    assumeFramebufferBinding(0);
    setViewport(0, 0, size);
}

mbgl::Size GLFWGLBackend::getSize() const {
    return size;
}

void GLFWGLBackend::setSize(const mbgl::Size newSize) {
    if (size == newSize) {
        return;
    }
    size = newSize;
    if (panSnapshot && panSnapshot->size != newSize) {
        activate();
        releasePanSnapshot();
    }
    if (glfwGetCurrentContext() == window) {
        updateAssumedState();
    }
}

void GLFWGLBackend::swap() {
    MLN_TRACE_FUNC();

    if (panSnapshotCaptureBeforeSwap) {
        activate();
        if (!panSnapshot) {
            panSnapshot = std::make_unique<PanSnapshot>();
        }
        setFramebufferBinding(0);
        setViewport(0, 0, size);
        panSnapshot->captureFromBackBuffer(size);
        panSnapshotCaptureBeforeSwap = false;
    }

    glfwSwapBuffers(window);
}

void GLFWGLBackend::setVSyncEnabled(bool enabled) {
    MLN_TRACE_FUNC();

    vsyncEnabled = enabled;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(enabled ? 1 : 0);
}

void GLFWGLBackend::requestPanSnapshotCaptureOnNextSwap() {
    panSnapshotCaptureBeforeSwap = true;
}

bool GLFWGLBackend::captureDisplayedPanSnapshot() {
    MLN_TRACE_FUNC();

    activate();
    if (!panSnapshot) {
        panSnapshot = std::make_unique<PanSnapshot>();
    }
    setFramebufferBinding(0);
    setViewport(0, 0, size);
    return panSnapshot->captureFromDisplayedBuffer(size);
}

bool GLFWGLBackend::drawPanSnapshot(const float offsetX, const float offsetY) {
    MLN_TRACE_FUNC();

    activate();
    if (!panSnapshot || !panSnapshot->texture) {
        return false;
    }
    if (panSnapshot->size != size) {
        releasePanSnapshot();
        return false;
    }

    setFramebufferBinding(0);
    setViewport(0, 0, size);
    panSnapshot->draw(size, offsetX, offsetY);
    glfwSwapBuffers(window);
    resetRendererStateAfterExternalDraw();
    return true;
}

void GLFWGLBackend::releasePanSnapshot() {
    activate();
    panSnapshotCaptureBeforeSwap = false;
    if (panSnapshot) {
        panSnapshot->clearSnapshot();
    }
}

bool GLFWGLBackend::hasPanSnapshot() const {
    return panSnapshot && panSnapshot->texture != 0;
}

namespace mbgl {
namespace gfx {

template <>
std::unique_ptr<GLFWBackend> Backend::Create<mbgl::gfx::Backend::Type::OpenGL>(GLFWwindow* window, bool capFrameRate) {
    MLN_TRACE_FUNC();

    return std::make_unique<GLFWGLBackend>(window, capFrameRate);
}

} // namespace gfx
} // namespace mbgl
