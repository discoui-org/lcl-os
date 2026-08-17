#!/usr/bin/env python3
"""Standalone PySide6 viewer for the bundled Pixel emulator skins.

This renders an isolated local SPICE GL scanout in a Pixel skin display rect.
It does not forward input.
"""

from __future__ import annotations

import re
import signal
import sys
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QImage, QPainter, QPixmap
from PySide6.QtWidgets import QApplication, QMainWindow, QWidget

from qemu_runner import QemuLaunchError, QemuRunner
from qmp_client import QmpTouchClient
from spice_gl_widget import SpiceGlWidget
from spice_client import SpiceScanoutProbe


DEFAULT_SKIN_DIR = Path(__file__).resolve().parent / "skins" / "pixel_8_pro"


class SkinLayoutError(ValueError):
    """The selected skin is not in the supported Pixel layout format."""


@dataclass(frozen=True)
class PixelSkinLayout:
    canvas_width: int
    canvas_height: int
    portrait_origin: QPointF
    display_rect: QRectF
    background_path: Path
    foreground_path: Path | None


def _named_block(source: str, name: str, start: int = 0) -> str:
    """Return one brace-delimited named block from the Android Pixel skin DSL."""
    match = re.search(rf"\b{re.escape(name)}\s*\{{", source[start:])
    if match is None:
        raise SkinLayoutError(f"Missing '{name}' block")

    opening = start + match.end() - 1
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise SkinLayoutError(f"Unclosed '{name}' block")


def _property(block: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s+([^\s{{}}]+)", block)
    if match is None:
        raise SkinLayoutError(f"Missing '{name}' property")
    return match.group(1)


def _integer_property(block: str, name: str) -> int:
    value = _property(block, name)
    try:
        return int(value)
    except ValueError as error:
        raise SkinLayoutError(f"'{name}' must be an integer, got {value!r}") from error


def _layout_part(layout_block: str, part_name: str) -> str:
    """Find the Pixel skin part block whose name is ``part_name``."""
    for match in re.finditer(r"\bpart\d+\s*\{", layout_block):
        opening = match.end() - 1
        depth = 0
        for index in range(opening, len(layout_block)):
            if layout_block[index] == "{":
                depth += 1
            elif layout_block[index] == "}":
                depth -= 1
                if depth == 0:
                    candidate = layout_block[opening + 1:index]
                    if _property(candidate, "name") == part_name:
                        return candidate
                    break
    raise SkinLayoutError(f"Portrait layout has no '{part_name}' part")


def parse_pixel_skin_layout(skin_dir: Path) -> PixelSkinLayout:
    """Parse the portrait-only layout structure used by bundled Pixel skins."""
    layout_path = skin_dir / "layout"
    try:
        source = layout_path.read_text(encoding="utf-8")
    except OSError as error:
        raise SkinLayoutError(f"Cannot read {layout_path}: {error}") from error

    parts = _named_block(source, "parts")
    device = _named_block(parts, "device")
    display = _named_block(device, "display")
    portrait = _named_block(parts, "portrait")
    background = _named_block(portrait, "background")

    foreground_path: Path | None = None
    try:
        foreground = _named_block(portrait, "foreground")
        foreground_path = skin_dir / _property(foreground, "mask")
    except SkinLayoutError:
        pass

    layouts = _named_block(source, "layouts")
    portrait_layout = _named_block(layouts, "portrait")
    portrait_part = _layout_part(portrait_layout, "portrait")
    device_part = _layout_part(portrait_layout, "device")

    display_x = _integer_property(display, "x") + _integer_property(device_part, "x")
    display_y = _integer_property(display, "y") + _integer_property(device_part, "y")
    display_rect = QRectF(
        display_x,
        display_y,
        _integer_property(display, "width"),
        _integer_property(display, "height"),
    )

    background_path = skin_dir / _property(background, "image")
    if not background_path.is_file():
        raise SkinLayoutError(f"Missing portrait background: {background_path}")
    if foreground_path is not None and not foreground_path.is_file():
        raise SkinLayoutError(f"Missing portrait foreground: {foreground_path}")

    return PixelSkinLayout(
        canvas_width=_integer_property(portrait_layout, "width"),
        canvas_height=_integer_property(portrait_layout, "height"),
        portrait_origin=QPointF(
            _integer_property(portrait_part, "x"),
            _integer_property(portrait_part, "y"),
        ),
        display_rect=display_rect,
        background_path=background_path,
        foreground_path=foreground_path,
    )


class DeviceViewer(QWidget):
    def __init__(self, layout: PixelSkinLayout, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._layout = layout
        self._background = QPixmap(str(layout.background_path))
        self._foreground = QPixmap(str(layout.foreground_path)) if layout.foreground_path else QPixmap()
        if self._background.isNull():
            raise SkinLayoutError(f"Cannot decode {layout.background_path}")
        if layout.foreground_path and self._foreground.isNull():
            raise SkinLayoutError(f"Cannot decode {layout.foreground_path}")

        self._display_widget = SpiceGlWidget(self._display_mask_image(), self)

        self.setMinimumSize(260, 480)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAutoFillBackground(False)

    def submit_spice_draw(self, channel, scanout) -> None:
        self._display_widget.submit_draw(channel, scanout)

    def release_gl_resources(self) -> None:
        self._display_widget.release_resources()

    def resizeEvent(self, event) -> None:  # type: ignore[override]
        super().resizeEvent(event)
        self._update_display_widget_geometry()

    def _display_mask_image(self) -> QImage:
        display = self._layout.display_rect
        if self._foreground.isNull():
            return QImage()
        if self._foreground.width() == int(display.width()) and self._foreground.height() == int(display.height()):
            return self._foreground.toImage()
        origin = self._layout.portrait_origin
        return self._foreground.copy(
            int(display.x() - origin.x()),
            int(display.y() - origin.y()),
            int(display.width()),
            int(display.height()),
        ).toImage()

    def _update_display_widget_geometry(self) -> None:
        canvas = QRectF(0, 0, self._layout.canvas_width, self._layout.canvas_height)
        scale = min(self.width() / canvas.width(), self.height() / canvas.height())
        display = self._layout.display_rect
        self._display_widget.setGeometry(
            round((self.width() - canvas.width() * scale) * 0.5 + display.x() * scale),
            round((self.height() - canvas.height() * scale) * 0.5 + display.y() * scale),
            round(display.width() * scale),
            round(display.height() * scale),
        )

    def paintEvent(self, event) -> None:  # type: ignore[override]
        del event
        painter = QPainter(self)
        painter.setCompositionMode(QPainter.CompositionMode_Source)
        painter.fillRect(self.rect(), Qt.transparent)
        painter.setCompositionMode(QPainter.CompositionMode_SourceOver)
        painter.setRenderHint(QPainter.SmoothPixmapTransform)

        canvas = QRectF(0, 0, self._layout.canvas_width, self._layout.canvas_height)
        scale = min(self.width() / canvas.width(), self.height() / canvas.height())
        target = QRectF(
            (self.width() - canvas.width() * scale) * 0.5,
            (self.height() - canvas.height() * scale) * 0.5,
            canvas.width() * scale,
            canvas.height() * scale,
        )

        portrait_origin = self._layout.portrait_origin
        background_rect = QRectF(
            target.x() + portrait_origin.x() * scale,
            target.y() + portrait_origin.y() * scale,
            self._background.width() * scale,
            self._background.height() * scale,
        )
        painter.drawPixmap(background_rect, self._background, QRectF(self._background.rect()))

        self._update_display_widget_geometry()


def main() -> int:
    app = QApplication(sys.argv)
    signal.signal(signal.SIGINT, lambda _signum, _frame: app.quit())
    signal_timer = QTimer()
    signal_timer.timeout.connect(lambda: None)
    signal_timer.start(100)
    try:
        layout = parse_pixel_skin_layout(DEFAULT_SKIN_DIR)
        viewer = DeviceViewer(layout)
    except SkinLayoutError as error:
        print(f"LCL Device Viewer: {error}", file=sys.stderr)
        return 1

    window = QMainWindow()
    window.setWindowTitle("LCL Device Viewer — Pixel 8 Pro")
    window.setWindowFlags(Qt.Window | Qt.FramelessWindowHint)
    window.setAttribute(Qt.WA_TranslucentBackground)
    window.setCentralWidget(viewer)
    window.setFixedSize(460, 940)
    window.show()

    runner = QemuRunner()
    qmp_touch = QmpTouchClient(runner.qmp_socket_path)
    spice_probe = SpiceScanoutProbe(runner.spice_socket_path, viewer.submit_spice_draw)
    spice_timer = QTimer()
    spice_timer.timeout.connect(spice_probe.pump)
    spice_timer.setTimerType(Qt.PreciseTimer)
    qmp_timer = QTimer()
    qmp_timer.timeout.connect(qmp_touch.pump)
    qmp_timer.setTimerType(Qt.PreciseTimer)
    try:
        runner.start()
    except QemuLaunchError as error:
        print(f"LCL Device Viewer: QEMU launch failed: {error}", file=sys.stderr, flush=True)
    else:
        spice_probe.start()
        spice_timer.start(8)
        qmp_timer.start(8)

    try:
        return app.exec()
    finally:
        spice_timer.stop()
        qmp_timer.stop()
        qmp_touch.stop()
        spice_probe.stop()
        viewer.release_gl_resources()
        signal_timer.stop()
        runner.stop()


if __name__ == "__main__":
    raise SystemExit(main())
