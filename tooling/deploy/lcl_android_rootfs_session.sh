#!/system/bin/sh
# Launch canonical glibc LCL userspace inside the mounted ARM64 rootfs.

ROOTFS_MOUNT=${LCL_ROOTFS_MOUNT:-/data/local/tmp/lcl-rootfs}
RUNTIME_DIR=${LCL_RUNTIME_DIR:-/data/local/tmp/lcl-runtime}
SESSION_SCRIPT="$RUNTIME_DIR/lcl-session-start.sh"

if [ ! -x "$ROOTFS_MOUNT/System/Core/lcl-sessiond" ] || \
   [ ! -x "$ROOTFS_MOUNT/System/Core/lcl-shell-launcher" ]; then
    echo "[LCL ROOTFS] Canonical session binaries are missing under $ROOTFS_MOUNT" >&2
    exit 1
fi

cat > "$SESSION_SCRIPT" <<'EOF'
#!/System/Tools/bash
export PATH=/System/Core:/System/Tools
export HOME=/Users/Rei
export USER=Rei
export TERM=xterm-256color
export LD_LIBRARY_PATH=/System/Library/Libraries

SESSIOND_PID=""
SANDBOXD_PID=""
SHELL_PID=""

cleanup() {
    if [ -n "$SHELL_PID" ]; then kill -TERM "$SHELL_PID" 2>/dev/null || true; fi
    if [ -n "$SESSIOND_PID" ]; then kill -TERM "$SESSIOND_PID" 2>/dev/null || true; fi
    if [ -n "$SANDBOXD_PID" ]; then kill -TERM "$SANDBOXD_PID" 2>/dev/null || true; fi
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[LCL SESSION] Starting canonical rootfs userspace..."
rm -f /Runtime/lcl-sessiond.sock /Runtime/lcl-sandboxd.sock

# sandboxd is a separate root-owned authority.  Its 0600 control socket is
# exclusively for the root session daemon; third-party applications never
# receive this endpoint.  On an Android kernel that cannot enforce the
# mandatory Linux profile, sandboxd fails closed.  That disables third-party
# launches but must not prevent the trusted shell/session from starting.
if [ -x /System/Core/lcl-sandboxd ]; then
    /System/Core/lcl-sandboxd --session-uid 0 --session-gid 0 > /Runtime/lcl-sandboxd.log 2>&1 &
    SANDBOXD_PID=$!

    for ((i=0; i<100; i++)); do
        if /System/Tools/grep -F /Runtime/lcl-sandboxd.sock /proc/net/unix |
           /System/Tools/grep -q "00010000 0005 01"; then
            break
        fi
        if ! kill -0 "$SANDBOXD_PID" 2>/dev/null; then
            SANDBOXD_PID=""
            break
        fi
        sleep 0.05
    done
fi

if ! /System/Tools/grep -F /Runtime/lcl-sandboxd.sock /proc/net/unix |
     /System/Tools/grep -q "00010000 0005 01"; then
    if [ -n "$SANDBOXD_PID" ]; then
        kill -TERM "$SANDBOXD_PID" 2>/dev/null || true
        wait "$SANDBOXD_PID" 2>/dev/null || true
        SANDBOXD_PID=""
    fi
    echo "[LCL SESSION] lcl-sandboxd is unavailable; third-party launches remain disabled" >&2
    echo "[LCL SESSION] see /Runtime/lcl-sandboxd.log for the fail-closed reason" >&2
fi

/System/Core/lcl-sessiond > /Runtime/lcl-sessiond.log 2>&1 &
SESSIOND_PID=$!

for ((i=0; i<100; i++)); do
    if /System/Tools/grep -F /Runtime/lcl-sessiond.sock /proc/net/unix |
       /System/Tools/grep -q "00010000 0005 01"; then
        break
    fi
    if ! kill -0 "$SESSIOND_PID" 2>/dev/null; then
        echo "[LCL SESSION] lcl-sessiond exited before its socket became ready" >&2
        exit 1
    fi
    sleep 0.05
done

if ! /System/Tools/grep -F /Runtime/lcl-sessiond.sock /proc/net/unix |
     /System/Tools/grep -q "00010000 0005 01"; then
    echo "[LCL SESSION] timed out waiting for lcl-sessiond.sock" >&2
    exit 1
fi

/System/Core/lcl-shell-launcher > /Runtime/lcl-shell.log 2>&1 &
SHELL_PID=$!
echo "[LCL SESSION] Gestalt-selected shell started (pid=$SHELL_PID)"

wait "$SESSIOND_PID" "$SHELL_PID"
EOF

chmod 0755 "$SESSION_SCRIPT"
exec chroot "$ROOTFS_MOUNT" /System/Tools/bash /Runtime/lcl-session-start.sh
