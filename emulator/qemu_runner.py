"""Launch and own the LCL QEMU process used by the device viewer."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parent.parent
RUN_QEMU = ROOT_DIR / "scripts" / "run_qemu.py"
REQUIRED_ARTIFACTS = (
    ROOT_DIR / "build" / "qemu-cache" / "vmlinuz",
    ROOT_DIR / "build" / "initramfs.cpio.gz",
    ROOT_DIR / "build" / "rootfs" / "lcl-rootfs-x86_64.ext4",
)


class QemuLaunchError(RuntimeError):
    """The viewer could not start its own QEMU process."""


@dataclass(frozen=True)
class SpiceEndpoint:
    """Local-only SPICE endpoint reserved for one viewer instance."""

    socket_path: Path


@dataclass(frozen=True)
class QmpEndpoint:
    """Local-only QMP endpoint reserved for one viewer instance."""

    socket_path: Path


class QemuRunner:
    """Own a mobile LCL QEMU process and its private viewer sockets."""

    def __init__(
        self,
        *,
        render_node: Path | None,
        build: bool = False,
        rebuild: bool = False,
        gestalt_path: Path | None = None,
    ) -> None:
        self._runtime_dir = Path(tempfile.mkdtemp(prefix="lcl-viewer-"))
        self.endpoint = SpiceEndpoint(self._runtime_dir / "spice.sock")
        self.qmp_endpoint = QmpEndpoint(self._runtime_dir / "qmp.sock")
        self._process: subprocess.Popen[bytes] | None = None
        self._stopping = False
        self._build = build
        self._rebuild = rebuild
        self._gestalt_path = gestalt_path
        self._render_node = render_node

    @property
    def spice_socket_path(self) -> Path:
        """The endpoint a future display client should connect to."""
        return self.endpoint.socket_path

    @property
    def qmp_socket_path(self) -> Path:
        """The private QMP endpoint used only for viewer touch injection."""
        return self.qmp_endpoint.socket_path

    def start(self) -> None:
        if self._process is not None:
            raise QemuLaunchError("Viewer QEMU is already running")

        missing = [str(path) for path in REQUIRED_ARTIFACTS if not path.is_file()]
        if missing and not self._build:
            self.stop()
            raise QemuLaunchError("Missing required LCL artifacts: " + ", ".join(missing))
        if self._render_node is None:
            self.stop()
            raise QemuLaunchError(
                "Could not resolve the DRM render node backing the viewer EGL context"
            )
        if not self._render_node.is_char_device():
            self.stop()
            raise QemuLaunchError(f"Viewer DRM render node is unavailable: {self._render_node}")

        # The directory is private to this runner.  Remove only this exact
        # endpoint in case a previous launch in the same instance left it behind.
        self._remove_runtime_files()
        self._stopping = False
        command = [
            sys.executable,
            str(RUN_QEMU),
            "--run",
            "--arch",
            "x86_64",
            "--mobile",
            "--gpu",
            "--spice-unix",
            str(self.spice_socket_path),
            "--qmp-unix",
            str(self.qmp_socket_path),
            "--runtime-dir",
            str(self._runtime_dir),
            "--render-node",
            str(self._render_node),
        ]
        if self._build:
            if self._rebuild:
                command.append("--rebuild")
        else:
            command.append("--no-build")
        if self._gestalt_path is not None:
            command.extend(["--gestalt", str(self._gestalt_path)])
        try:
            self._process = subprocess.Popen(
                command,
                cwd=ROOT_DIR,
            )
        except OSError as error:
            self.stop()
            raise QemuLaunchError(f"Could not launch QEMU: {error}") from error

        if self._process.poll() is not None:
            exit_code = self._process.returncode
            print(
                f"LCL Device Viewer: QEMU exited with code {exit_code}",
                file=sys.stderr,
                flush=True,
            )
            self.stop()
            raise QemuLaunchError(f"QEMU launcher exited immediately ({exit_code})")

        print(f"LCL Device Viewer: QEMU PID {self._process.pid}", flush=True)
        print(f"LCL Device Viewer: SPICE socket {self.spice_socket_path}", flush=True)
        print(f"LCL Device Viewer: QMP socket {self.qmp_socket_path}", flush=True)
        print("LCL Device Viewer: QEMU launch started", flush=True)
        threading.Thread(
            target=self._report_unexpected_exit,
            args=(self._process,),
            daemon=True,
        ).start()

    def stop(self) -> None:
        self._stopping = True
        process = self._process
        self._process = None
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self._remove_runtime_files()
        try:
            self._runtime_dir.rmdir()
        except OSError:
            pass

    def _remove_runtime_files(self) -> None:
        for runtime_path in (
            self.spice_socket_path,
            self.qmp_socket_path,
            self._runtime_dir / "gestalt.json",
        ):
            try:
                runtime_path.unlink()
            except FileNotFoundError:
                pass

    def _report_unexpected_exit(self, process: subprocess.Popen[bytes]) -> None:
        exit_code = process.wait()
        if not self._stopping:
            print(
                f"LCL Device Viewer: QEMU exited with code {exit_code}",
                file=sys.stderr,
                flush=True,
            )
