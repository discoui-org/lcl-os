"""Non-blocking private QMP client for viewer-originated multitouch events."""

from __future__ import annotations

import errno
import json
import socket
import sys
from collections import deque
from pathlib import Path
from typing import Any


QEMU_ABSOLUTE_MAX = 0x7FFF


class QmpTouchClient:
    """Pump the viewer's private QMP socket from Qt's existing event loop."""

    def __init__(self, socket_path: Path) -> None:
        self._socket_path = socket_path
        self._socket: socket.socket | None = None
        self._receive_buffer = bytearray()
        self._send_buffer = bytearray()
        self._pending_touch_messages: deque[dict[str, Any]] = deque()
        self._ready = False
        self._capabilities_requested = False
        self._connect_error_logged = False
        self._protocol_error_logged = False

    def pump(self) -> None:
        if self._socket is None:
            self._connect()
            return
        self._receive()
        self._flush()

    def stop(self) -> None:
        if self._socket is not None:
            try:
                self._socket.close()
            except OSError:
                pass
        self._socket = None
        self._ready = False
        self._send_buffer.clear()
        self._pending_touch_messages.clear()

    def send_contacts(self, events: list[dict[str, Any]]) -> None:
        """Queue one atomic QMP input-send-event contact batch."""
        if not events:
            return
        message = {
            "execute": "input-send-event",
            "arguments": {"events": events},
        }
        if not self._ready:
            self._pending_touch_messages.append(message)
            return
        self._queue(message)
        if self._socket is not None:
            self._flush()

    def _connect(self) -> None:
        if not self._socket_path.exists():
            return
        candidate = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        candidate.setblocking(False)
        result = candidate.connect_ex(str(self._socket_path))
        if result not in (0, errno.EINPROGRESS, errno.EALREADY, errno.EISCONN):
            candidate.close()
            if not self._connect_error_logged:
                self._connect_error_logged = True
                print(f"LCL Device Viewer: QMP connect failed: errno={result}", file=sys.stderr, flush=True)
            return
        self._socket = candidate

    def _receive(self) -> None:
        assert self._socket is not None
        while True:
            try:
                chunk = self._socket.recv(4096)
            except BlockingIOError:
                break
            except OSError as error:
                self._disconnect_with_error(f"read failed: {error}")
                return
            if not chunk:
                self._disconnect_with_error("peer closed connection")
                return
            self._receive_buffer.extend(chunk)
        while b"\n" in self._receive_buffer:
            raw_line, _, remainder = self._receive_buffer.partition(b"\n")
            self._receive_buffer = bytearray(remainder)
            try:
                message = json.loads(raw_line)
            except json.JSONDecodeError:
                self._disconnect_with_error("invalid JSON greeting/response")
                return
            if "QMP" in message and not self._capabilities_requested:
                self._capabilities_requested = True
                self._queue({"execute": "qmp_capabilities"})
                continue
            if self._capabilities_requested and "return" in message and not self._ready:
                self._ready = True
                print("LCL Device Viewer: QMP touch channel ready", flush=True)
                while self._pending_touch_messages:
                    self._queue(self._pending_touch_messages.popleft())
            elif "error" in message and not self._protocol_error_logged:
                self._protocol_error_logged = True
                print(f"LCL Device Viewer: QMP command failed: {message['error']}", file=sys.stderr, flush=True)

    def _queue(self, message: dict[str, Any]) -> None:
        self._send_buffer.extend(json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n")

    def _flush(self) -> None:
        if self._socket is None or not self._send_buffer:
            return
        try:
            sent = self._socket.send(self._send_buffer)
        except BlockingIOError:
            return
        except OSError as error:
            self._disconnect_with_error(f"write failed: {error}")
            return
        del self._send_buffer[:sent]

    def _disconnect_with_error(self, detail: str) -> None:
        if not self._connect_error_logged:
            self._connect_error_logged = True
            print(f"LCL Device Viewer: QMP connection failed: {detail}", file=sys.stderr, flush=True)
        self.stop()
