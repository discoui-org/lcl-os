"""SPICE GL scanout metadata probe for the device viewer.

This module deliberately observes the SPICE display channel only. It neither
imports the DMA-BUF nor copies any framebuffer into a CPU image.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Any, Callable

from spice_gl_widget import ScanoutMetadata

try:
    import gi
except ImportError as error:
    GLib = GObject = Spice = None  # type: ignore[assignment]
    _DEPENDENCY_ERROR = f"missing Python GI dependency 'python-gobject': {error}"
else:
    try:
        gi.require_version("SpiceClientGLib", "2.0")
        from gi.repository import GLib, GObject, SpiceClientGLib as Spice
    except (ImportError, ValueError) as error:
        GLib = GObject = Spice = None  # type: ignore[assignment]
        _DEPENDENCY_ERROR = (
            "missing 'SpiceClientGLib-2.0' typelib from spice-gtk: "
            f"{error}"
        )
    else:
        _DEPENDENCY_ERROR = None


class SpiceScanoutProbe:
    """Connect to one local SPICE socket and log GL scanout metadata."""

    def __init__(
        self,
        socket_path: Path,
        submit_draw: Callable[[Any, ScanoutMetadata], None] | None = None,
    ) -> None:
        self._socket_path = socket_path
        self._submit_draw = submit_draw
        self._session: Any | None = None
        self._socket_wait_warning_logged = False
        self._pump_diagnostics_until: float | None = None
        self._next_pump_diagnostic: float | None = None
        self._connection_attempted = False
        self._display_logged = False
        self._observed_channels: set[tuple[int, int]] = set()
        self._channel_connect_initiated: set[tuple[int, int]] = set()
        self._draw_count = 0
        self._scanout_logged = False

    def start(self) -> None:
        if _DEPENDENCY_ERROR is not None:
            print(f"LCL Device Viewer: SPICE unavailable: {_DEPENDENCY_ERROR}", file=sys.stderr, flush=True)
            return
        now = time.monotonic()
        self._pump_diagnostics_until = now + 3.0
        self._next_pump_diagnostic = now
        print(f"LCL Device Viewer: SPICE unix-path {self._socket_path}", flush=True)

    def pump(self) -> None:
        """Run pending GLib I/O work from the Qt event loop."""
        if _DEPENDENCY_ERROR is not None:
            return
        if not self._connection_attempted:
            if not self._socket_path.exists():
                # `main.py qemu --mobile` may be packaging artifacts in Docker
                # before it execs QEMU. The viewer process is still healthy;
                # do not permanently abandon its private endpoint after an
                # arbitrary short timeout.
                if not self._socket_wait_warning_logged and self._pump_diagnostics_until is not None and time.monotonic() >= self._pump_diagnostics_until:
                    print(
                        "LCL Device Viewer: waiting for QEMU SPICE socket: "
                        f"{self._socket_path}",
                        flush=True,
                    )
                    self._socket_wait_warning_logged = True
                self._log_pump_diagnostic()
                return
            self._connect()

        context = GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
        self._log_pump_diagnostic()

    def stop(self) -> None:
        if self._session is not None:
            try:
                Spice.Session.disconnect(self._session)
            except Exception as error:
                print(f"LCL Device Viewer: SPICE disconnect failed: {error}", file=sys.stderr, flush=True)
            self._session = None

    def _connect(self) -> None:
        self._connection_attempted = True
        try:
            self._session = Spice.Session(
                unix_path=str(self._socket_path),
                gl_scanout=True,
                enable_audio=False,
                enable_usbredir=False,
            )
            GObject.Object.connect(self._session, "channel-new", self._on_channel_new)
            if self._session.connect():
                print("LCL Device Viewer: SPICE connect initiated", flush=True)
                self._observe_existing_channels()
            else:
                print("LCL Device Viewer: SPICE connection failed: session rejected connect", file=sys.stderr, flush=True)
        except Exception as error:
            self._session = None
            print(f"LCL Device Viewer: SPICE connection failed: {error}", file=sys.stderr, flush=True)

    def _on_channel_new(self, _session: Any, channel: Any) -> None:
        self._observe_channel(channel)

    def _observe_existing_channels(self) -> None:
        if self._session is None:
            return
        for channel in self._session.get_channels():
            self._observe_channel(channel)

    def _observe_channel(self, channel: Any) -> None:
        channel_type = int(channel.get_property("channel-type"))
        channel_id = int(channel.get_property("channel-id"))
        key = (channel_type, channel_id)
        if key in self._observed_channels:
            return
        self._observed_channels.add(key)

        type_name = Spice.Channel.type_to_string(channel_type) or "unknown"
        print(
            f"LCL Device Viewer: SPICE channel new: type={type_name} ({channel_type}) id={channel_id}",
            flush=True,
        )
        GObject.Object.connect(channel, "channel-event", self._on_channel_event)
        if isinstance(channel, Spice.DisplayChannel):
            GObject.Object.connect(channel, "gl-draw", self._on_gl_draw)
            if not self._display_logged:
                self._display_logged = True
                print("LCL Device Viewer: display channel ready", flush=True)

        # SpiceSession.connect() owns the main channel. Child channels received
        # through channel-new need their own SpiceChannel connection request.
        if type_name != "main":
            self._connect_child_channel(channel, key, type_name, channel_id)

    def _connect_child_channel(
        self,
        channel: Any,
        key: tuple[int, int],
        type_name: str,
        channel_id: int,
    ) -> None:
        if key in self._channel_connect_initiated:
            return
        self._channel_connect_initiated.add(key)
        try:
            # This is the GI invocation of spice_channel_connect(channel), not
            # GObject's signal-binding method.
            started = Spice.Channel.connect(channel)
        except Exception as error:
            print(
                "LCL Device Viewer: SPICE channel connect failed: "
                f"type={type_name} id={channel_id} exception={error}",
                file=sys.stderr,
                flush=True,
            )
            return

        if started:
            print(
                f"LCL Device Viewer: SPICE channel connect initiated: type={type_name} id={channel_id}",
                flush=True,
            )
            return

        print(
            "LCL Device Viewer: SPICE channel connect failed: "
            f"type={type_name} id={channel_id} return={started} error={self._channel_error(channel)}",
            file=sys.stderr,
            flush=True,
        )

    def _on_channel_event(self, channel: Any, event: Any) -> None:
        if event == Spice.ChannelEvent.OPENED:
            channel_type = int(channel.get_property("channel-type"))
            channel_id = int(channel.get_property("channel-id"))
            type_name = Spice.Channel.type_to_string(channel_type) or "unknown"
            print(
                f"LCL Device Viewer: SPICE channel opened: type={type_name} ({channel_type}) id={channel_id}",
                flush=True,
            )
            return

        if int(event) >= int(Spice.ChannelEvent.ERROR_CONNECT):
            detail = self._channel_error(channel, fallback=event.value_nick)
            print(f"LCL Device Viewer: SPICE connection failed: {detail}", file=sys.stderr, flush=True)

    @staticmethod
    def _channel_error(channel: Any, fallback: str = "none") -> str:
        try:
            error = channel.get_error()
            return error.message if error is not None else fallback
        except Exception as error:
            return f"{fallback}; get_error failed: {error}"

    def _log_pump_diagnostic(self) -> None:
        if self._next_pump_diagnostic is None or self._pump_diagnostics_until is None:
            return
        now = time.monotonic()
        if now < self._next_pump_diagnostic or now > self._pump_diagnostics_until:
            return
        self._next_pump_diagnostic = now + 1.0
        channels = len(self._session.get_channels()) if self._session is not None else 0
        print(f"LCL Device Viewer: GLib pump alive, channels={channels}", flush=True)

    def _on_gl_draw(self, channel: Any, _x: int, _y: int, _width: int, _height: int) -> None:
        self._draw_count += 1
        duplicated_fd: int | None = None
        ownership_transferred = False
        try:
            scanout = channel.get_gl_scanout()
            if scanout is None:
                print(f"LCL Device Viewer: GL draw #{self._draw_count} without scanout", flush=True)
                return

            try:
                duplicated_fd = os.dup(int(scanout.fd))
            except OSError as error:
                print(
                    f"LCL Device Viewer: DMA-BUF fd duplication failed: {error}",
                    file=sys.stderr,
                    flush=True,
                )
                return

            metadata = ScanoutMetadata(
                fd=duplicated_fd,
                width=int(scanout.width),
                height=int(scanout.height),
                stride=int(scanout.stride),
                pixel_format=int(scanout.format),
                y0top=bool(scanout.y0top),
            )
            if not self._scanout_logged:
                self._scanout_logged = True
                print(
                    "LCL Device Viewer: GL scanout "
                    f"fd={metadata.fd} width={metadata.width} height={metadata.height} stride={metadata.stride} "
                    f"format=0x{metadata.pixel_format:08x} y0top={metadata.y0top}",
                    flush=True,
                )
            if self._submit_draw is None:
                print(f"LCL Device Viewer: GL draw #{self._draw_count}", flush=True)
            else:
                self._submit_draw(channel, metadata)
                ownership_transferred = True
        except Exception as error:
            print(f"LCL Device Viewer: GL draw handoff failed: {error}", file=sys.stderr, flush=True)
        finally:
            if not ownership_transferred:
                if duplicated_fd is not None:
                    try:
                        os.close(duplicated_fd)
                    except OSError:
                        pass
                try:
                    channel.gl_draw_done()
                except Exception:
                    pass
