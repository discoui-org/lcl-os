#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

const char* gStage = "startup";

void report(const char* stage) {
    gStage = stage;
    std::cout << "lcl_client_gpu_probe stage=" << stage << std::endl;
}

void reportErrno(const char* stage) {
    std::cerr << "lcl_client_gpu_probe " << stage << " errno=" << errno
              << " (" << std::strerror(errno) << ")\n";
}

void reportEgl(const char* stage) {
    std::cerr << "lcl_client_gpu_probe " << stage << " egl=0x" << std::hex
              << eglGetError() << std::dec << "\n";
}

void crashHandler(int signalNumber) {
    std::cerr << "lcl_client_gpu_probe fatal_signal=" << signalNumber
              << " stage=" << gStage << "\n";
    _exit(128 + signalNumber);
}

int openRenderNode() {
    for (int index = 128; index <= 143; ++index) {
        const std::string path = "/dev/dri/renderD" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            std::cout << "lcl_client_gpu_probe render_node=" << path << std::endl;
            return fd;
        }
    }
    reportErrno("open_render_node_failed");
    return -1;
}

} // namespace

int main() {
    // sandboxd intentionally routes an app's stdio to /dev/null. Keep this
    // integration probe observable without widening that production policy:
    // its report stays in the probe's own sandboxed /Data directory.
    const int reportFd = open("/Data/gpu-transport-probe.log",
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (reportFd >= 0) {
        (void)dup2(reportFd, STDOUT_FILENO);
        (void)dup2(reportFd, STDERR_FILENO);
        if (reportFd > STDERR_FILENO) close(reportFd);
    }
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGBUS, crashHandler);
    std::signal(SIGILL, crashHandler);
    std::signal(SIGSEGV, crashHandler);

    report("open_render_node");
    const int renderFd = openRenderNode();
    if (renderFd < 0) return 10;

    report("gbm_create_device");
    gbm_device* gbm = gbm_create_device(renderFd);
    if (!gbm) {
        std::cerr << "lcl_client_gpu_probe gbm_create_device_failed\n";
        close(renderFd);
        return 11;
    }
    std::cout << "lcl_client_gpu_probe gbm_backend="
              << gbm_device_get_backend_name(gbm) << std::endl;

    const EGLint noAttributes[] = {EGL_NONE};
    const EGLAttrib noModernAttributes[] = {EGL_NONE};
    const auto getPlatformDisplayExt =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    const auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYPROC>(
            eglGetProcAddress("eglGetPlatformDisplay"));

    report("egl_get_display");
    EGLDisplay display = EGL_NO_DISPLAY;
    if (getPlatformDisplayExt) {
        display = getPlatformDisplayExt(EGL_PLATFORM_GBM_KHR, gbm, noAttributes);
    }
    if (display == EGL_NO_DISPLAY && getPlatformDisplay) {
        display = getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, noModernAttributes);
    }
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm));
    }
    if (display == EGL_NO_DISPLAY) {
        reportEgl("egl_get_display_failed");
        gbm_device_destroy(gbm);
        close(renderFd);
        return 12;
    }

    report("egl_initialize");
    if (!eglInitialize(display, nullptr, nullptr)) {
        reportEgl("egl_initialize_failed");
        gbm_device_destroy(gbm);
        close(renderFd);
        return 13;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        reportEgl("egl_bind_es_failed");
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(renderFd);
        return 14;
    }

    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    const bool surfaceless = extensions &&
        std::strstr(extensions, "EGL_KHR_surfaceless_context") != nullptr;
    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    report("egl_choose_config");
    if (!eglChooseConfig(display, configAttributes, &config, 1, &configCount) ||
        configCount != 1) {
        reportEgl("egl_choose_config_failed");
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(renderFd);
        return 15;
    }

    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    report("egl_create_context");
    EGLContext context = eglCreateContext(
        display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        reportEgl("egl_create_context_failed");
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(renderFd);
        return 16;
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (!surfaceless) {
        const EGLint pbufferAttributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
        report("egl_create_pbuffer");
        surface = eglCreatePbufferSurface(display, config, pbufferAttributes);
        if (surface == EGL_NO_SURFACE) {
            reportEgl("egl_create_pbuffer_failed");
            eglDestroyContext(display, context);
            eglTerminate(display);
            gbm_device_destroy(gbm);
            close(renderFd);
            return 17;
        }
    }
    report("egl_make_current");
    if (!eglMakeCurrent(display, surface, surface, context)) {
        reportEgl("egl_make_current_failed");
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(renderFd);
        return 18;
    }
    std::cout << "lcl_client_gpu_probe renderer="
              << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << std::endl;

    report("gbm_bo_create");
    gbm_bo* buffer = gbm_bo_create(
        gbm, 64, 64, GBM_FORMAT_ARGB8888,
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!buffer) {
        std::cerr << "lcl_client_gpu_probe gbm_bo_create_failed\n";
        return 19;
    }
    report("gbm_bo_get_fd");
    const int dmaBufFd = gbm_bo_get_fd(buffer);
    if (dmaBufFd < 0) {
        reportErrno("gbm_bo_get_fd_failed");
        gbm_bo_destroy(buffer);
        return 20;
    }
    std::cout << "lcl_client_gpu_probe dmabuf_export=yes stride="
              << gbm_bo_get_stride(buffer) << " modifier=0x" << std::hex
              << gbm_bo_get_modifier(buffer) << std::dec << std::endl;

    close(dmaBufFd);
    gbm_bo_destroy(buffer);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    gbm_device_destroy(gbm);
    close(renderFd);
    std::cout << "lcl_client_gpu_probe result=pass" << std::endl;
    return 0;
}
