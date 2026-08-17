"""Linux-only SPICE GL scanout presentation without CPU framebuffer copies."""

from __future__ import annotations

import ctypes
import fcntl
import os
import sys
from collections import deque
from dataclasses import dataclass
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QImage, QSurfaceFormat
from PySide6.QtOpenGL import (
    QOpenGLBuffer,
    QOpenGLShader,
    QOpenGLShaderProgram,
    QOpenGLTexture,
    QOpenGLVertexArrayObject,
)
from PySide6.QtOpenGLWidgets import QOpenGLWidget


EGL_EXTENSIONS = 0x3055
EGL_DRAW = 0x3059
EGL_HEIGHT = 0x3056
EGL_WIDTH = 0x3057
EGL_NONE = 0x3038
EGL_LINUX_DMA_BUF_EXT = 0x3270
EGL_LINUX_DRM_FOURCC_EXT = 0x3271
EGL_DMA_BUF_PLANE0_FD_EXT = 0x3272
EGL_DMA_BUF_PLANE0_OFFSET_EXT = 0x3273
EGL_DMA_BUF_PLANE0_PITCH_EXT = 0x3274

GL_TEXTURE_2D = 0x0DE1
GL_TEXTURE0 = 0x84C0
GL_TEXTURE_MIN_FILTER = 0x2801
GL_TEXTURE_MAG_FILTER = 0x2800
GL_LINEAR = 0x2601
GL_CLAMP_TO_EDGE = 0x812F
GL_TEXTURE_WRAP_S = 0x2802
GL_TEXTURE_WRAP_T = 0x2803
GL_RGBA = 0x1908
GL_COLOR_BUFFER_BIT = 0x00004000
GL_BLEND = 0x0BE2
GL_ZERO = 0
GL_ONE_MINUS_SRC_ALPHA = 0x0303
GL_TRIANGLE_STRIP = 0x0005
GL_FLOAT = 0x1406
GL_NO_ERROR = 0


def _safe_log(message: str, *, error: bool = False) -> None:
    """Best-effort diagnostics must never interrupt Qt's GL paint callback."""
    try:
        print(message, file=sys.stderr if error else sys.stdout, flush=True)
    except Exception:
        pass


_safe_log(f"LCL Device Viewer: spice_gl_widget loaded from {__file__}")


@dataclass(frozen=True)
class ScanoutMetadata:
    """One GL draw's metadata; ``fd`` is a viewer-owned duplicate."""

    fd: int
    width: int
    height: int
    stride: int
    pixel_format: int
    y0top: bool


@dataclass
class PendingDraw:
    channel: Any
    scanout: ScanoutMetadata
    finished: bool = False

    def finish(self) -> None:
        if self.finished:
            return
        self.finished = True
        try:
            os.close(self.scanout.fd)
        except OSError:
            pass
        try:
            self.channel.gl_draw_done()
        except Exception as error:
            _safe_log(f"LCL Device Viewer: gl_draw_done failed: {error}", error=True)


class EglImportError(RuntimeError):
    def __init__(self, message: str, *, capability_failure: bool = False) -> None:
        super().__init__(message)
        self.capability_failure = capability_failure


class EglImporterInitializationError(EglImportError):
    def __init__(self, stage: str, error: BaseException) -> None:
        self.stage = stage
        self.original_error_type = type(error).__name__
        self.original_error_message = str(error)
        super().__init__(
            "EGL importer initialization FAILED "
            f"stage={stage} error={self.original_error_type}: {self.original_error_message}"
        )


class EglDmaBufImporter:
    """Resolve the EGL extension entrypoints against Qt's current context."""

    def __init__(self) -> None:
        stage = "load-libEGL"

        def set_stage(value: str) -> None:
            nonlocal stage
            stage = value

        try:
            self._egl = self._load_egl(set_stage)

            stage = "obtain-current-display"
            self.display = self._egl.eglGetCurrentDisplay()
            stage = "obtain-current-context"
            self.context = self._egl.eglGetCurrentContext()
            stage = "obtain-current-draw-surface"
            self.draw_surface = self._egl.eglGetCurrentSurface(EGL_DRAW)
            self._log_current_state("initializeGL", self.display, self.context, self.draw_surface)

            stage = "eglQueryString"
            raw_extensions = self._egl.eglQueryString(self.display, EGL_EXTENSIONS) if self.display else None
            self.extension_string = (raw_extensions or b"").decode("ascii", errors="replace")
            self.extensions = self.extension_string.split()

            stage = "resolve-eglCreateImageKHR"
            self._create_image = self._resolve(
                b"eglCreateImageKHR",
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_uint,
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_int),
                required=False,
            )
            stage = "resolve-eglDestroyImageKHR"
            self._destroy_image = self._resolve(
                b"eglDestroyImageKHR",
                ctypes.c_uint,
                ctypes.c_void_p,
                ctypes.c_void_p,
                required=False,
            )
            stage = "resolve-glEGLImageTargetTexture2DOES"
            self._image_target_texture = self._resolve(
                b"glEGLImageTargetTexture2DOES",
                None,
                ctypes.c_uint,
                ctypes.c_void_p,
                required=False,
            )
        except EglImportError:
            raise
        except Exception as error:
            raise EglImporterInitializationError(stage, error) from None

        self.valid_display = bool(self.display)
        self.dma_buf_extension_advertised = "EGL_EXT_image_dma_buf_import" in self.extensions
        self.create_image_resolved = self._create_image is not None
        self.destroy_image_resolved = self._destroy_image is not None
        self.image_target_resolved = self._image_target_texture is not None

        self.capabilities_available = (
            self.valid_display
            and self.dma_buf_extension_advertised
            and self.create_image_resolved
            and self.destroy_image_resolved
            and self.image_target_resolved
        )
        if not self.capabilities_available:
            self.log_capabilities()
            capability_summary = (
                f"display={self._yes_no(self.valid_display)} "
                f"extension={self._yes_no(self.dma_buf_extension_advertised)} "
                f"createImage={self._yes_no(self.create_image_resolved)} "
                f"destroyImage={self._yes_no(self.destroy_image_resolved)} "
                f"imageTarget={self._yes_no(self.image_target_resolved)}"
            )
            raise EglImportError(
                f"EGL DMA-BUF import unsupported: {capability_summary}",
                capability_failure=True,
            )

    def log_capabilities(self) -> None:
        _safe_log("LCL Device Viewer: EGL DMA-BUF capabilities:")
        _safe_log(f"  display={self._yes_no(self.valid_display)}")
        _safe_log(f"  extension={self._yes_no(self.dma_buf_extension_advertised)}")
        _safe_log(f"  eglCreateImageKHR={self._yes_no(self.create_image_resolved)}")
        _safe_log(f"  eglDestroyImageKHR={self._yes_no(self.destroy_image_resolved)}")
        _safe_log(f"  glEGLImageTargetTexture2DOES={self._yes_no(self.image_target_resolved)}")

        if not self.dma_buf_extension_advertised:
            _safe_log(
                "LCL Device Viewer: EGL display extensions: "
                f"{self.extension_string if self.extension_string else '<empty>'}"
            )

        for symbol, resolved in (
            ("eglCreateImageKHR", self.create_image_resolved),
            ("eglDestroyImageKHR", self.destroy_image_resolved),
            ("glEGLImageTargetTexture2DOES", self.image_target_resolved),
        ):
            if not resolved:
                _safe_log(
                    "LCL Device Viewer: EGL symbol resolve: "
                    f"method=eglGetProcAddress symbol={symbol} result=no "
                    "directLookup=not-attempted"
                )

    @staticmethod
    def _yes_no(value: bool) -> str:
        return "yes" if value else "no"

    @staticmethod
    def _load_egl(set_stage: Any | None = None) -> Any:
        def stage(value: str) -> None:
            if set_stage is not None:
                set_stage(value)

        stage("load-libEGL")
        egl = ctypes.CDLL("libEGL.so.1")
        stage("resolve-eglGetCurrentDisplay")
        egl.eglGetCurrentDisplay.restype = ctypes.c_void_p
        stage("resolve-eglGetCurrentContext")
        egl.eglGetCurrentContext.restype = ctypes.c_void_p
        stage("resolve-eglGetCurrentSurface")
        egl.eglGetCurrentSurface.argtypes = (ctypes.c_int,)
        egl.eglGetCurrentSurface.restype = ctypes.c_void_p
        stage("resolve-eglGetError")
        egl.eglGetError.restype = ctypes.c_uint
        stage("resolve-eglQueryString")
        egl.eglQueryString.argtypes = (ctypes.c_void_p, ctypes.c_int)
        egl.eglQueryString.restype = ctypes.c_char_p
        stage("resolve-eglGetProcAddress")
        egl.eglGetProcAddress.argtypes = (ctypes.c_char_p,)
        egl.eglGetProcAddress.restype = ctypes.c_void_p
        return egl

    @staticmethod
    def _handle_text(value: int | None, no_value: str) -> str:
        return no_value if not value else f"0x{int(value):x}"

    @classmethod
    def _current_state(cls, egl: Any, phase: str) -> tuple[int | None, int | None, int | None]:
        display = egl.eglGetCurrentDisplay()
        context = egl.eglGetCurrentContext()
        draw_surface = egl.eglGetCurrentSurface(EGL_DRAW)
        cls._log_current_state(phase, display, context, draw_surface)
        return display, context, draw_surface

    @classmethod
    def _log_current_state(
        cls,
        phase: str,
        display: int | None,
        context: int | None,
        draw_surface: int | None,
    ) -> None:
        _safe_log(
            f"LCL Device Viewer: EGL current [{phase}]: "
            f"display={cls._handle_text(display, 'EGL_NO_DISPLAY')} "
            f"context={cls._handle_text(context, 'EGL_NO_CONTEXT')} "
            f"drawSurface={cls._handle_text(draw_surface, 'EGL_NO_SURFACE')}",
        )

    @classmethod
    def log_current_state(cls, phase: str) -> None:
        cls._current_state(cls._load_egl(), phase)

    def _resolve(
        self,
        name: bytes,
        restype: Any,
        *argtypes: Any,
        diagnostic: bool = False,
        required: bool = True,
    ) -> Any | None:
        address = self._egl.eglGetProcAddress(name)
        if diagnostic:
            _safe_log(
                f"LCL Device Viewer: {name.decode()} resolve: {'yes' if address else 'no'}",
            )
        if not address:
            if required:
                raise EglImportError(f"EGL symbol unavailable: {name.decode()}")
            return None
        return ctypes.CFUNCTYPE(restype, *argtypes)(address)

    def create_image(self, scanout: ScanoutMetadata) -> ctypes.c_void_p:
        attributes = (ctypes.c_int * 13)(
            EGL_DMA_BUF_PLANE0_FD_EXT,
            scanout.fd,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            scanout.stride,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_WIDTH,
            scanout.width,
            EGL_HEIGHT,
            scanout.height,
            EGL_LINUX_DRM_FOURCC_EXT,
            scanout.pixel_format,
            EGL_NONE,
        )
        image = self._create_image(
            self.display,
            ctypes.c_void_p(),
            EGL_LINUX_DMA_BUF_EXT,
            ctypes.c_void_p(),
            attributes,
        )
        if not image:
            raise EglImportError(
                f"EGLImage import FAILED eglError=0x{self._egl.eglGetError():04x}"
            )
        return ctypes.c_void_p(image)

    def destroy_image(self, image: ctypes.c_void_p) -> None:
        if image and not self._destroy_image(self.display, image):
            _safe_log(
                f"LCL Device Viewer: eglDestroyImageKHR failed: 0x{self._egl.eglGetError():04x}",
                error=True,
            )

    def bind_image_to_texture(self, image: ctypes.c_void_p) -> None:
        self._image_target_texture(GL_TEXTURE_2D, image)


class SpiceGlWidget(QOpenGLWidget):
    """Display-local QOpenGLWidget for SPICE scanout DMA-BUFs."""

    def __init__(self, mask: QImage, parent=None) -> None:
        super().__init__(parent)
        surface_format = QSurfaceFormat()
        surface_format.setAlphaBufferSize(8)
        surface_format.setRenderableType(QSurfaceFormat.OpenGL)
        surface_format.setVersion(3, 3)
        surface_format.setProfile(QSurfaceFormat.CoreProfile)
        self.setFormat(surface_format)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAttribute(Qt.WA_NoSystemBackground, False)
        self.setAttribute(Qt.WA_AlwaysStackOnTop)
        self.setAutoFillBackground(False)
        # We redraw the whole display from the presentation texture each time.
        # Do not retain a partially composited transparent backing FBO.
        self.setUpdateBehavior(QOpenGLWidget.NoPartialUpdate)

        self._mask_image = mask
        self._pending_draws: deque[PendingDraw] = deque()
        self._egl_importer: EglDmaBufImporter | None = None
        self._program: QOpenGLShaderProgram | None = None
        self._mask_program: QOpenGLShaderProgram | None = None
        self._flip_y_uniform_location = -1
        self._image_uniform_location = -1
        self._mask_image_uniform_location = -1
        self._quad: QOpenGLBuffer | None = None
        self._vao: QOpenGLVertexArrayObject | None = None
        self._mask_texture: QOpenGLTexture | None = None
        self._scanout_texture: QOpenGLTexture | None = None
        self._presentation_texture: QOpenGLTexture | None = None
        self._framebuffer_width = 0
        self._framebuffer_height = 0
        self._egl_image: ctypes.c_void_p | None = None
        self._loaded_scanout: ScanoutMetadata | None = None
        self._gpu_active_logged = False
        self._import_success_logged = False
        self._quad_drawn_logged = False
        self._texture_bind_logged = False
        self._importer_initialized_logged = False
        self._initialize_logged = False
        self._paint_count = 0
        self._paint_egl_state_logged = False
        self._initialization_error: EglImportError | None = None
        self._initialization_error_logged = False
        self._reported_import_error: tuple[str, int, int, int, int, bool] | None = None
        self._initialized_context: Any | None = None
        self._accepting_draws = True
        self._gpu_rendering_active = False
        self._last_import_signature: tuple[int, int, int, int, bool] | None = None
    def submit_draw(self, channel: Any, scanout: ScanoutMetadata) -> None:
        """Queue one SPICE draw; completion happens from paintGL after drawing."""
        pending = PendingDraw(channel, scanout)
        if not self._accepting_draws:
            pending.finish()
            return
        self._pending_draws.append(pending)
        try:
            self.update()
        except Exception:
            # The accepted draw remains queued and owns its duplicated fd.
            pass

    def initializeGL(self) -> None:  # type: ignore[override]
        if not self._initialize_logged:
            self._initialize_logged = True
            _safe_log("LCL Device Viewer: QOpenGLWidget initializeGL")
            self._log_qt_context()
        current_context = self.context()
        if self._initialized_context is not None and self._initialized_context is not current_context:
            self._reset_recreated_context_state()
        self._initialized_context = current_context
        self._initialization_error = None
        self._initialization_error_logged = False
        self._initialize_egl_resources()

    def _reset_recreated_context_state(self) -> None:
        # initializeGL() means Qt has already made the replacement context
        # current. Old-context handles cannot be deleted through this context;
        # discard them and rebuild the same resources below.
        self._egl_importer = None
        self._program = None
        self._mask_program = None
        self._flip_y_uniform_location = -1
        self._image_uniform_location = -1
        self._mask_image_uniform_location = -1
        self._quad = None
        self._vao = None
        self._mask_texture = None
        self._scanout_texture = None
        self._presentation_texture = None
        self._egl_image = None
        self._loaded_scanout = None
        self._importer_initialized_logged = False

    def _initialize_egl_resources(self) -> bool:
        if self._egl_importer is not None:
            return True
        if self._initialization_error is not None:
            return False

        stage = "initialize-EGL-importer"
        try:
            importer = EglDmaBufImporter()
            self._egl_importer = importer
            if not self._importer_initialized_logged:
                self._importer_initialized_logged = True
                _safe_log("LCL Device Viewer: EGL importer initialized")
            importer.log_capabilities()
            stage = "create-shader-GL-resources"
            self._initialize_quad()
            try:
                self._initialize_mask_texture()
            except Exception as error:
                self._mask_texture = None
                _safe_log(f"LCL Device Viewer: mask texture unavailable: {error}", error=True)
            return True
        except EglImporterInitializationError as error:
            self._egl_importer = None
            self._initialization_error = error
            self._report_import_error(error, None)
        except EglImportError as error:
            self._egl_importer = None
            if stage == "initialize-EGL-importer":
                self._initialization_error = error
            else:
                self._initialization_error = EglImporterInitializationError(stage, error)
            self._report_import_error(self._initialization_error, None)
        except Exception as error:
            self._egl_importer = None
            self._initialization_error = EglImporterInitializationError(stage, error)
            self._report_import_error(self._initialization_error, None)
        return False

    def resizeGL(self, width: int, height: int) -> None:  # type: ignore[override]
        self._framebuffer_width = width
        self._framebuffer_height = height
        self.context().functions().glViewport(0, 0, width, height)

    def paintGL(self) -> None:  # type: ignore[override]
        functions = self.context().functions()
        # The parent window is translucent, so stale GL state must never keep
        # the display FBO's alpha channel write-disabled between frames.
        functions.glColorMask(True, True, True, True)
        functions.glDisable(GL_BLEND)
        self._paint_count += 1
        if self._paint_count <= 5:
            _safe_log(f"LCL Device Viewer: paintGL #{self._paint_count}")
        if not self._paint_egl_state_logged:
            self._paint_egl_state_logged = True
            EglDmaBufImporter.log_current_state("paintGL")

        if not self._pending_draws:
            if self._presentation_texture is not None:
                functions.glClearColor(0.0, 0.0, 0.0, 1.0)
                functions.glClear(GL_COLOR_BUFFER_BIT)
                self._draw_texture(self._presentation_texture, False)
                self._apply_mask()
                functions.glFinish()
                return
            functions.glClearColor(0.0, 0.0, 0.0, 1.0)
            functions.glClear(GL_COLOR_BUFFER_BIT)
            return

        if self._egl_importer is None and self._initialization_error is None:
            self._initialize_egl_resources()

        while self._pending_draws:
            pending = self._pending_draws.popleft()
            try:
                self._ensure_scanout_texture(pending.scanout)
                # The physical display is opaque; mask.webp removes only its
                # explicit cutout pixels after the guest quad is drawn.
                functions.glClearColor(0.0, 0.0, 0.0, 1.0)
                functions.glClear(GL_COLOR_BUFFER_BIT)
                if self._draw_scanout(pending.scanout.y0top):
                    self._capture_presentation_texture()
                    self._gpu_rendering_active = True
                    if not self._quad_drawn_logged:
                        self._quad_drawn_logged = True
                        _safe_log("LCL Device Viewer: DMA-BUF quad drawn")
                    if not self._gpu_active_logged:
                        self._gpu_active_logged = True
                        _safe_log("LCL Device Viewer: SPICE DMA-BUF GPU rendering active")
                self._apply_mask()
                # The channel may recycle its DMA-BUF as soon as gl_draw_done()
                # returns. Complete this correctness-first frame before giving
                # that ownership back to spice-gtk.
                functions.glFinish()
            except EglImportError as error:
                self._report_import_error(error, pending.scanout)
            except Exception as error:
                self._report_import_error(EglImportError(str(error)), pending.scanout)
            finally:
                try:
                    self._release_scanout_texture()
                except Exception as error:
                    _safe_log(f"LCL Device Viewer: GPU resource cleanup failed: {error}", error=True)
                finally:
                    pending.finish()

    def _log_qt_context(self) -> None:
        context = self.context()
        context_exists = context is not None
        context_valid = bool(context_exists and context.isValid())
        fmt = context.format() if context_exists else self.format()
        renderable_type = fmt.renderableType()
        profile = fmt.profile()
        physical_width = round(self.width() * self.devicePixelRatioF())
        physical_height = round(self.height() * self.devicePixelRatioF())

        def enum_text(value: Any) -> str:
            """PySide6 enum wrappers are not necessarily int-convertible."""
            name = getattr(value, "name", None)
            numeric_value = getattr(value, "value", None)
            if name is not None and numeric_value is not None:
                return f"{name}({numeric_value})"
            return str(value)

        _safe_log(
            "LCL Device Viewer: QOpenGLWidget context "
            f"present={'yes' if context_exists else 'no'} "
            f"valid={'yes' if context_valid else 'no'} "
            f"renderableType={enum_text(renderable_type)} "
            f"profile={enum_text(profile)} "
            f"version={fmt.majorVersion()}.{fmt.minorVersion()} "
            f"alphaBufferSize={fmt.alphaBufferSize()} "
            f"defaultFramebufferObject={self.defaultFramebufferObject()} "
            f"physicalSize={physical_width}x{physical_height}",
        )
        if fmt.alphaBufferSize() <= 0:
            _safe_log("LCL Device Viewer: QOpenGLWidget alpha buffer: none")

    def _require_egl_importer(self) -> EglDmaBufImporter:
        if self._egl_importer is None and self._initialization_error is None:
            self._initialize_egl_resources()
        if self._egl_importer is not None:
            return self._egl_importer
        if self._initialization_error is not None:
            raise self._initialization_error

        error = EglImporterInitializationError(
            "initialize-EGL-importer",
            RuntimeError("initialization attempt returned without an importer or error"),
        )
        self._initialization_error = error
        self._report_import_error(error, None)
        raise error

    def release_resources(self) -> None:
        self._accepting_draws = False
        self._finish_pending_draws()
        if not self.context() or not self.context().isValid():
            return
        self.makeCurrent()
        self._release_scanout_texture()
        if self._mask_texture is not None:
            self._mask_texture.destroy()
            self._mask_texture = None
        if self._presentation_texture is not None:
            self._presentation_texture.destroy()
            self._presentation_texture = None
        if self._quad is not None:
            self._quad.destroy()
            self._quad = None
        if self._vao is not None:
            self._vao.destroy()
            self._vao = None
        self.doneCurrent()

    def closeEvent(self, event: Any) -> None:  # type: ignore[override]
        self._accepting_draws = False
        self._finish_pending_draws()
        super().closeEvent(event)

    def _finish_pending_draws(self) -> None:
        while self._pending_draws:
            self._pending_draws.popleft().finish()

    def _initialize_quad(self) -> None:
        self._program = self._make_program(
            """
            #version 330 core
            layout(location = 0) in vec2 position;
            layout(location = 1) in vec2 texcoord;
            out vec2 uv;
            uniform bool flip_y;
            void main() {
                gl_Position = vec4(position, 0.0, 1.0);
                uv = vec2(texcoord.x, flip_y ? 1.0 - texcoord.y : texcoord.y);
            }
            """,
            """
            #version 330 core
            in vec2 uv;
            uniform sampler2D image;
            out vec4 color;
            void main() {
                vec3 guest_rgb = texture(image, uv).rgb;
                color = vec4(guest_rgb, 1.0);
            }
            """,
        )
        self._mask_program = self._make_program(
            """
            #version 330 core
            layout(location = 0) in vec2 position;
            layout(location = 1) in vec2 texcoord;
            out vec2 uv;
            void main() {
                gl_Position = vec4(position, 0.0, 1.0);
                // QImage rows are top-down while this OpenGL quad's texture
                // origin is bottom-left. Keep the skin mask in portrait space.
                uv = vec2(texcoord.x, 1.0 - texcoord.y);
            }
            """,
            """
            #version 330 core
            in vec2 uv;
            uniform sampler2D mask_image;
            out vec4 color;
            void main() { color = vec4(0.0, 0.0, 0.0, texture(mask_image, uv).r); }
            """,
        )
        self._flip_y_uniform_location = self._program.uniformLocation("flip_y")
        self._image_uniform_location = self._program.uniformLocation("image")
        self._mask_image_uniform_location = self._mask_program.uniformLocation("mask_image")
        for name, location in (
            ("flip_y", self._flip_y_uniform_location),
            ("image", self._image_uniform_location),
            ("mask_image", self._mask_image_uniform_location),
        ):
            if location < 0:
                raise EglImportError(f"shader uniform unavailable: {name}")

        # position.xy, texture-coordinate.xy; no guest pixels are copied here.
        vertices = ctypes.c_float * 16
        vertex_data = vertices(
            -1.0, -1.0, 0.0, 0.0,
             1.0, -1.0, 1.0, 0.0,
            -1.0,  1.0, 0.0, 1.0,
             1.0,  1.0, 1.0, 1.0,
        )
        self._vao = QOpenGLVertexArrayObject()
        self._vao.create()
        self._vao.bind()
        self._quad = QOpenGLBuffer(QOpenGLBuffer.VertexBuffer)
        self._quad.create()
        self._quad.bind()
        self._quad.allocate(bytes(vertex_data), ctypes.sizeof(vertex_data))
        self._program.bind()
        self._program.enableAttributeArray(0)
        self._program.setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * ctypes.sizeof(ctypes.c_float))
        self._program.enableAttributeArray(1)
        self._program.setAttributeBuffer(1, GL_FLOAT, 2 * ctypes.sizeof(ctypes.c_float), 2, 4 * ctypes.sizeof(ctypes.c_float))
        self._program.release()
        self._quad.release()
        self._vao.release()

    @staticmethod
    def _make_program(vertex_source: str, fragment_source: str) -> QOpenGLShaderProgram:
        program = QOpenGLShaderProgram()
        if not program.addShaderFromSourceCode(QOpenGLShader.Vertex, vertex_source):
            raise EglImportError(program.log())
        if not program.addShaderFromSourceCode(QOpenGLShader.Fragment, fragment_source):
            raise EglImportError(program.log())
        if not program.link():
            raise EglImportError(program.log())
        return program

    def _initialize_mask_texture(self) -> None:
        if self._mask_image.isNull():
            return
        mask_values = self._mask_image.convertToFormat(QImage.Format_Alpha8)
        if not mask_values.reinterpretAsFormat(QImage.Format_Grayscale8):
            raise EglImportError("mask alpha conversion failed")
        mask_texture_image = mask_values.convertToFormat(QImage.Format_RGBA8888)

        self._mask_texture = QOpenGLTexture(mask_texture_image)
        self._mask_texture.setMinificationFilter(QOpenGLTexture.Linear)
        self._mask_texture.setMagnificationFilter(QOpenGLTexture.Linear)
        self._mask_texture.setWrapMode(QOpenGLTexture.ClampToEdge)

    def _ensure_scanout_texture(self, scanout: ScanoutMetadata) -> None:
        # A gl-draw grants temporary access to the current SPICE GL resource.
        # Do not assume a reused fd has the same backing allocation: re-import
        # every accepted draw until the channel exposes a safe reuse contract.
        import_signature = (
            scanout.width,
            scanout.height,
            scanout.stride,
            scanout.pixel_format,
            scanout.y0top,
        )
        log_import = import_signature != self._last_import_signature
        if log_import:
            self._last_import_signature = import_signature
            _safe_log(
                "LCL Device Viewer: EGLImage import "
                f"fd={scanout.fd} width={scanout.width} height={scanout.height} "
                f"stride={scanout.stride} fourcc=0x{scanout.pixel_format:08x} "
                f"y0top={scanout.y0top}",
            )
        self._release_scanout_texture()
        importer = self._require_egl_importer()
        try:
            fcntl.fcntl(scanout.fd, fcntl.F_GETFD)
        except OSError as error:
            raise EglImportError(
                "duplicated DMA-BUF fd became invalid before import"
            ) from error
        image = importer.create_image(scanout)
        if not self._import_success_logged:
            self._import_success_logged = True
            _safe_log("LCL Device Viewer: EGLImage import OK")
        texture: QOpenGLTexture | None = None
        try:
            texture = QOpenGLTexture(QOpenGLTexture.Target2D)
            texture.create()
            if not texture.isCreated():
                raise EglImportError("EGLImage texture bind FAILED glError=0x0000 texture-created=no")
            functions = self.context().functions()
            functions.glBindTexture(GL_TEXTURE_2D, texture.textureId())
            functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
            functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
            functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)
            functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)
            while functions.glGetError() != GL_NO_ERROR:
                pass
            importer.bind_image_to_texture(image)
            gl_error = functions.glGetError()
            functions.glBindTexture(GL_TEXTURE_2D, 0)
            if gl_error != GL_NO_ERROR:
                raise EglImportError(f"EGLImage texture bind FAILED glError=0x{gl_error:04x}")
            if not self._texture_bind_logged:
                self._texture_bind_logged = True
                _safe_log(
                    "LCL Device Viewer: EGL texture created=yes, "
                    "glEGLImageTargetTexture2DOES called=yes",
                )
                _safe_log("LCL Device Viewer: EGLImage texture bind OK")
        except Exception:
            if texture is not None:
                texture.destroy()
            importer.destroy_image(image)
            raise

        self._egl_image = image
        self._scanout_texture = texture
        self._loaded_scanout = scanout

    def _draw_scanout(self, y0top: bool) -> bool:
        if self._scanout_texture is None:
            return False
        return self._draw_texture(self._scanout_texture, not y0top)

    def _draw_texture(self, texture: QOpenGLTexture, flip_y: bool) -> bool:
        if self._program is None or self._vao is None:
            return False
        functions = self.context().functions()
        self._vao.bind()
        self._program.bind()
        functions.glUniform1i(self._flip_y_uniform_location, 1 if flip_y else 0)
        functions.glUniform1i(self._image_uniform_location, 0)
        functions.glActiveTexture(GL_TEXTURE0)
        functions.glBindTexture(GL_TEXTURE_2D, texture.textureId())
        functions.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)
        functions.glBindTexture(GL_TEXTURE_2D, 0)
        self._program.release()
        self._vao.release()
        return True

    def _capture_presentation_texture(self) -> None:
        width = self._framebuffer_width
        height = self._framebuffer_height
        if width <= 0 or height <= 0:
            raise EglImportError("presentation texture size is unavailable")

        if self._presentation_texture is None:
            texture = QOpenGLTexture(QOpenGLTexture.Target2D)
            texture.create()
            if not texture.isCreated():
                raise EglImportError("presentation texture creation failed")
            self._presentation_texture = texture

        functions = self.context().functions()
        while functions.glGetError() != GL_NO_ERROR:
            pass
        functions.glBindTexture(GL_TEXTURE_2D, self._presentation_texture.textureId())
        functions.glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0)
        functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
        functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
        functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)
        functions.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)
        gl_error = functions.glGetError()
        functions.glBindTexture(GL_TEXTURE_2D, 0)
        if gl_error != GL_NO_ERROR:
            raise EglImportError(f"presentation texture copy failed glError=0x{gl_error:04x}")

    def _apply_mask(self) -> None:
        if self._mask_program is None or self._vao is None or self._mask_texture is None:
            return
        functions = self.context().functions()
        self._vao.bind()
        self._mask_program.bind()
        functions.glUniform1i(self._mask_image_uniform_location, 0)
        functions.glActiveTexture(GL_TEXTURE0)
        functions.glBindTexture(GL_TEXTURE_2D, self._mask_texture.textureId())
        functions.glEnable(GL_BLEND)
        functions.glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA)
        functions.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)
        functions.glDisable(GL_BLEND)
        functions.glBindTexture(GL_TEXTURE_2D, 0)
        self._mask_program.release()
        self._vao.release()

    def _release_scanout_texture(self) -> None:
        texture = self._scanout_texture
        image = self._egl_image
        importer = self._egl_importer
        self._scanout_texture = None
        self._egl_image = None
        self._loaded_scanout = None

        cleanup_error: Exception | None = None
        if texture is not None:
            try:
                texture.destroy()
            except Exception as error:
                cleanup_error = error
        if image is not None and importer is not None:
            try:
                importer.destroy_image(image)
            except Exception as error:
                if cleanup_error is None:
                    cleanup_error = error
        if cleanup_error is not None:
            raise cleanup_error

    def _report_import_error(self, error: EglImportError, scanout: ScanoutMetadata | None) -> None:
        if isinstance(error, EglImporterInitializationError) and scanout is not None:
            return
        if scanout is None:
            if error.capability_failure:
                # The detailed capability table has already been emitted by
                # the importer. Add the one-line failure with scanout metadata.
                return
            if self._initialization_error_logged:
                return
            self._initialization_error_logged = True
            _safe_log(f"LCL Device Viewer: {error}", error=True)
            return

        error_key = (
            str(error),
            scanout.width,
            scanout.height,
            scanout.stride,
            scanout.pixel_format,
            scanout.y0top,
        )
        if error_key == self._reported_import_error:
            return
        self._reported_import_error = error_key
        metadata = (
            f" fd={scanout.fd} width={scanout.width} height={scanout.height} "
            f"stride={scanout.stride} format=0x{scanout.pixel_format:08x} y0top={scanout.y0top}"
        )
        _safe_log(f"LCL Device Viewer: {error}{metadata}", error=True)
