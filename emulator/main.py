#!/usr/bin/env python3
"""Standalone PySide6 viewer for the bundled Pixel emulator skins.

This intentionally renders only a black display placeholder.  It does not
launch a guest or forward input; those integrations belong to a later layer.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QImage, QPainter, QPixmap
from PySide6.QtWidgets import QApplication, QMainWindow, QWidget


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

        self.setMinimumSize(260, 480)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAutoFillBackground(False)

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

        display = self._layout.display_rect
        display_layer = QImage(
            int(display.width()),
            int(display.height()),
            QImage.Format.Format_ARGB32_Premultiplied,
        )
        display_layer.fill(Qt.transparent)
        display_painter = QPainter(display_layer)
        display_painter.fillRect(display_layer.rect(), Qt.green)

        if not self._foreground.isNull():
            mask_source = QRectF(self._foreground.rect())
            if (
                self._foreground.width() != int(display.width())
                or self._foreground.height() != int(display.height())
            ):
                mask_source = QRectF(
                    display.x() - portrait_origin.x(),
                    display.y() - portrait_origin.y(),
                    display.width(),
                    display.height(),
                )
            display_painter.setCompositionMode(QPainter.CompositionMode_DestinationOut)
            display_painter.drawPixmap(QRectF(display_layer.rect()), self._foreground, mask_source)
            display_painter.setCompositionMode(QPainter.CompositionMode_SourceOver)

        display_painter.end()
        viewport = QRectF(
            target.x() + display.x() * scale,
            target.y() + display.y() * scale,
            display.width() * scale,
            display.height() * scale,
        )
        painter.drawImage(viewport, display_layer, QRectF(display_layer.rect()))


def main() -> int:
    app = QApplication(sys.argv)
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
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
