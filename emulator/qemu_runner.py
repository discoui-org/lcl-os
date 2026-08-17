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


class QemuRunner:
    """Own a mobile LCL QEMU process and its private SPICE Unix socket."""

    def __init__(self) -> None:
        self._runtime_dir = Path(tempfile.mkdtemp(prefix="lcl-viewer-"))
        self.endpoint = SpiceEndpoint(self._runtime_dir / "spice.sock")
        self._process: subprocess.Popen[bytes] | None = None
        self._stopping = False

    @property
    def spice_socket_path(self) -> Path:
        """The endpoint a future display client should connect to."""
        return self.endpoint.socket_path

    def start(self) -> None:
        if self._process is not None:
            raise QemuLaunchError("Viewer QEMU is already running")

        missing = [str(path) for path in REQUIRED_ARTIFACTS if not path.is_file()]
        if missing:
            self.stop()
            raise QemuLaunchError("Missing required LCL artifacts: " + ", ".join(missing))

        # The directory is private to this runner.  Remove only this exact
        # endpoint in case a previous launch in the same instance left it behind.
        self._remove_socket()
        self._stopping = False
        command = [
            sys.executable,
            str(RUN_QEMU),
            "--run",
            "--arch",
            "x86_64",
            "--mobile",
            "--gpu",
            "--no-build",
            "--spice-unix",
            str(self.spice_socket_path),
        ]
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
        self._remove_socket()
        try:
            self._runtime_dir.rmdir()
        except OSError:
            pass

    def _remove_socket(self) -> None:
        try:
            self.spice_socket_path.unlink()
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
