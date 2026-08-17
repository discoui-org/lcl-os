"""Translate Qt viewer input into physical QEMU keyboard and touch events."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QWidget

from qmp_client import QEMU_ABSOLUTE_MAX, QmpInputClient


# Linux evdev keycode -> QEMU QKeyCode. Using the physical code preserves the
# host keyboard layout: a Turkish-Q key still reaches the same physical guest
# key instead of being reinterpreted through a US character map.
_EVDEV_TO_QCODE = {
    1: "esc",
    2: "1", 3: "2", 4: "3", 5: "4", 6: "5",
    7: "6", 8: "7", 9: "8", 10: "9", 11: "0",
    12: "minus", 13: "equal", 14: "backspace", 15: "tab",
    16: "q", 17: "w", 18: "e", 19: "r", 20: "t",
    21: "y", 22: "u", 23: "i", 24: "o", 25: "p",
    26: "bracket_left", 27: "bracket_right", 28: "ret", 29: "ctrl",
    30: "a", 31: "s", 32: "d", 33: "f", 34: "g",
    35: "h", 36: "j", 37: "k", 38: "l", 39: "semicolon",
    40: "apostrophe", 41: "grave_accent", 42: "shift", 43: "backslash",
    44: "z", 45: "x", 46: "c", 47: "v", 48: "b",
    49: "n", 50: "m", 51: "comma", 52: "dot", 53: "slash",
    54: "shift_r", 55: "kp_multiply", 56: "alt", 57: "spc",
    58: "caps_lock", 59: "f1", 60: "f2", 61: "f3", 62: "f4",
    63: "f5", 64: "f6", 65: "f7", 66: "f8", 67: "f9",
    68: "f10", 69: "num_lock", 70: "scroll_lock",
    71: "kp_7", 72: "kp_8", 73: "kp_9", 74: "kp_subtract",
    75: "kp_4", 76: "kp_5", 77: "kp_6", 78: "kp_add",
    79: "kp_1", 80: "kp_2", 81: "kp_3", 82: "kp_0",
    83: "kp_decimal", 87: "f11", 88: "f12", 96: "kp_enter",
    97: "ctrl_r", 98: "kp_divide", 99: "sysrq", 100: "alt_r",
    102: "home", 103: "up", 104: "pgup", 105: "left",
    106: "right", 107: "end", 108: "down", 109: "pgdn",
    110: "insert", 111: "delete", 119: "pause",
    125: "meta_l", 126: "meta_r", 127: "menu",
}


_QT_KEY_TO_QCODE = {
    Qt.Key.Key_Escape: "esc",
    Qt.Key.Key_Tab: "tab",
    Qt.Key.Key_Backtab: "tab",
    Qt.Key.Key_Backspace: "backspace",
    Qt.Key.Key_Return: "ret",
    Qt.Key.Key_Enter: "kp_enter",
    Qt.Key.Key_Insert: "insert",
    Qt.Key.Key_Delete: "delete",
    Qt.Key.Key_Pause: "pause",
    Qt.Key.Key_Print: "print",
    Qt.Key.Key_SysReq: "sysrq",
    Qt.Key.Key_Home: "home",
    Qt.Key.Key_End: "end",
    Qt.Key.Key_Left: "left",
    Qt.Key.Key_Up: "up",
    Qt.Key.Key_Right: "right",
    Qt.Key.Key_Down: "down",
    Qt.Key.Key_PageUp: "pgup",
    Qt.Key.Key_PageDown: "pgdn",
    Qt.Key.Key_Shift: "shift",
    Qt.Key.Key_Control: "ctrl",
    Qt.Key.Key_Meta: "meta_l",
    Qt.Key.Key_Alt: "alt",
    Qt.Key.Key_AltGr: "alt_r",
    Qt.Key.Key_CapsLock: "caps_lock",
    Qt.Key.Key_NumLock: "num_lock",
    Qt.Key.Key_ScrollLock: "scroll_lock",
    Qt.Key.Key_Menu: "menu",
    Qt.Key.Key_Space: "spc",
    Qt.Key.Key_Minus: "minus",
    Qt.Key.Key_Equal: "equal",
    Qt.Key.Key_BracketLeft: "bracket_left",
    Qt.Key.Key_BracketRight: "bracket_right",
    Qt.Key.Key_Backslash: "backslash",
    Qt.Key.Key_Semicolon: "semicolon",
    Qt.Key.Key_Apostrophe: "apostrophe",
    Qt.Key.Key_QuoteLeft: "grave_accent",
    Qt.Key.Key_Comma: "comma",
    Qt.Key.Key_Period: "dot",
    Qt.Key.Key_Slash: "slash",
}
_QT_KEY_TO_QCODE.update({getattr(Qt.Key, f"Key_{letter}"): letter.lower() for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"})
_QT_KEY_TO_QCODE.update({getattr(Qt.Key, f"Key_{digit}"): digit for digit in "0123456789"})
_QT_KEY_TO_QCODE.update({getattr(Qt.Key, f"Key_F{number}"): f"f{number}" for number in range(1, 25)})


def qcode_for_key_event(event: QKeyEvent) -> str | None:
    """Resolve a QEMU physical key code, preferring Qt's native scan code."""
    native_scan_code = int(event.nativeScanCode())
    if native_scan_code > 0:
        platform_name = QApplication.platformName().lower()
        if platform_name == "xcb":
            # X11 keycodes use the evdev value plus the historical offset 8.
            candidates = (native_scan_code - 8, native_scan_code)
        else:
            # Wayland normally exposes the evdev value directly. Keep the
            # offset form as a fallback for other XKB-backed Qt plugins.
            candidates = (native_scan_code, native_scan_code - 8)
        for candidate in candidates:
            qcode = _EVDEV_TO_QCODE.get(candidate)
            if qcode is not None:
                return qcode
    return _QT_KEY_TO_QCODE.get(event.key())


def normalized_axis(position: float, extent: int) -> int:
    """Map one widget-local coordinate onto QEMU's 0..0x7fff ABS range."""
    if extent <= 1:
        return 0
    clamped = max(0.0, min(float(extent - 1), position))
    return round(clamped * QEMU_ABSOLUTE_MAX / float(extent - 1))


class ViewerInputBridge(QObject):
    """Forward display-widget input without exposing a desktop mouse device."""

    def __init__(self, target: QWidget, client: QmpInputClient) -> None:
        super().__init__(target)
        self._target = target
        self._client = client
        self._touch_active = False
        self._last_touch = (0, 0)
        self._pressed_qcodes: set[str] = set()

        target.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        target.setMouseTracking(True)
        target.installEventFilter(self)

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # type: ignore[override]
        if watched is not self._target:
            return False

        event_type = event.type()
        if event_type == QEvent.Type.MouseButtonPress:
            mouse_event = event
            assert isinstance(mouse_event, QMouseEvent)
            if mouse_event.button() == Qt.MouseButton.LeftButton:
                self._target.setFocus(Qt.FocusReason.MouseFocusReason)
                self._last_touch = self._touch_position(mouse_event)
                self._touch_active = True
                self._client.send_touch("begin", *self._last_touch)
                mouse_event.accept()
                return True
        elif event_type == QEvent.Type.MouseMove and self._touch_active:
            mouse_event = event
            assert isinstance(mouse_event, QMouseEvent)
            touch_position = self._touch_position(mouse_event)
            if touch_position != self._last_touch:
                self._last_touch = touch_position
                self._client.send_touch("update", *self._last_touch)
            mouse_event.accept()
            return True
        elif event_type == QEvent.Type.MouseButtonRelease:
            mouse_event = event
            assert isinstance(mouse_event, QMouseEvent)
            if mouse_event.button() == Qt.MouseButton.LeftButton and self._touch_active:
                self._last_touch = self._touch_position(mouse_event)
                self._client.send_touch("end", *self._last_touch)
                self._touch_active = False
                mouse_event.accept()
                return True
        elif event_type == QEvent.Type.KeyPress:
            key_event = event
            assert isinstance(key_event, QKeyEvent)
            qcode = qcode_for_key_event(key_event)
            if qcode is not None:
                # Repeated key-downs intentionally pass through; the guest
                # input layer owns repeat semantics just as it does on hardware.
                self._client.send_key(qcode, True)
                self._pressed_qcodes.add(qcode)
                key_event.accept()
                return True
        elif event_type == QEvent.Type.KeyRelease:
            key_event = event
            assert isinstance(key_event, QKeyEvent)
            qcode = qcode_for_key_event(key_event)
            if qcode is not None:
                if not key_event.isAutoRepeat():
                    self._client.send_key(qcode, False)
                    self._pressed_qcodes.discard(qcode)
                key_event.accept()
                return True
        elif event_type == QEvent.Type.FocusOut:
            self.release_all()

        return False

    def release_all(self) -> None:
        """Prevent stuck contacts/modifiers when the viewer loses focus."""
        if self._touch_active:
            self._client.send_touch("cancel", *self._last_touch)
            self._touch_active = False
        for qcode in sorted(self._pressed_qcodes):
            self._client.send_key(qcode, False)
        self._pressed_qcodes.clear()

    def _touch_position(self, event: QMouseEvent) -> tuple[int, int]:
        position = event.position()
        return (
            normalized_axis(position.x(), self._target.width()),
            normalized_axis(position.y(), self._target.height()),
        )
