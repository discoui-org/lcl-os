"""Non-blocking private QMP client for viewer-originated input events."""

from __future__ import annotations

import errno
import json
import socket
import sys
from collections import deque
from pathlib import Path
from typing import Any


QEMU_ABSOLUTE_MAX = 0x7FFF


class QmpInputClient:
    """Pump the viewer's private QMP socket from Qt's existing event loop."""

    def __init__(self, socket_path: Path) -> None:
        self._socket_path = socket_path
        self._socket: socket.socket | None = None
        self._receive_buffer = bytearray()
        self._send_buffer = bytearray()
        self._pending_input_messages: deque[dict[str, Any]] = deque()
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
        self._receive_buffer.clear()
        self._pending_input_messages.clear()
        self._capabilities_requested = False

    def send_events(self, events: list[dict[str, Any]]) -> None:
        """Queue one atomic QMP ``input-send-event`` batch."""
        if not events:
            return
        message = {
            "execute": "input-send-event",
            "arguments": {"events": events},
        }
        if not self._ready:
            self._pending_input_messages.append(message)
            return
        self._queue(message)
        if self._socket is not None:
            self._flush()

    def send_key(self, qcode: str, down: bool) -> None:
        """Send one physical keyboard transition through virtio-keyboard."""
        self.send_events([
            {
                "type": "key",
                "data": {
                    "down": down,
                    "key": {"type": "qcode", "data": qcode},
                },
            }
        ])

    def send_touch(self, phase: str, x: int, y: int) -> None:
        """Send one single-contact phase through virtio-multitouch."""
        if phase not in {"begin", "update", "end", "cancel"}:
            raise ValueError(f"Unsupported touch phase: {phase}")

        x = max(0, min(QEMU_ABSOLUTE_MAX, int(x)))
        y = max(0, min(QEMU_ABSOLUTE_MAX, int(y)))
        tracking_id = -1 if phase in {"end", "cancel"} else 0
        events: list[dict[str, Any]] = []
        if phase == "begin":
            events.extend([
                {
                    "type": "mtt",
                    "data": {
                        "type": "begin",
                        "slot": 0,
                        "tracking-id": tracking_id,
                        # QMP requires these fields for every mtt variant.
                        # QEMU ignores them for begin/end transitions.
                        "axis": "x",
                        "value": x,
                    },
                },
                {
                    "type": "btn",
                    "data": {"button": "touch", "down": True},
                },
            ])
        elif phase in {"end", "cancel"}:
            events.append({
                "type": "mtt",
                "data": {
                    "type": phase,
                    "slot": 0,
                    "tracking-id": tracking_id,
                    "axis": "x",
                    "value": x,
                },
            })

        if phase in {"begin", "update"}:
            # An active contact moves by reporting position data only. Sending
            # another tracking-id on every move would make LCL's evdev backend
            # interpret each frame as a fresh touch-down.
            events.extend([
                {
                    "type": "mtt",
                    "data": {
                        "type": "data",
                        "slot": 0,
                        "tracking-id": tracking_id,
                        "axis": "x",
                        "value": x,
                    },
                },
                {
                    "type": "mtt",
                    "data": {
                        "type": "data",
                        "slot": 0,
                        "tracking-id": tracking_id,
                        "axis": "y",
                        "value": y,
                    },
                },
            ])
        self.send_events(events)

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
                print("LCL Device Viewer: QMP input channel ready", flush=True)
                while self._pending_input_messages:
                    self._queue(self._pending_input_messages.popleft())
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


# Kept for compatibility with the first viewer-only QMP client revision.
QmpTouchClient = QmpInputClient
