#!/bin/bash
# vm/run.sh - Launch the xv6fs development VM
#
# The repo is shared at /mnt/src inside the guest via virtio-9p.
# Serial console is on your terminal.  Exit: Ctrl-A, X
#
# Environment overrides:
#   QEMU_MEM=4G  ./vm/run.sh      # default 2G
#   QEMU_SMP=4   ./vm/run.sh      # default 2

set -euo pipefail

VMDIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$VMDIR")"

for f in disk.img vmlinuz initrd.img; do
    if [[ ! -f "$VMDIR/$f" ]]; then
        echo "ERROR: $VMDIR/$f not found. Run ./vm/setup.sh first."
        exit 1
    fi
done

# Use KVM if available, otherwise fall back to software emulation
ACCEL="-accel tcg"
if [[ -w /dev/kvm ]]; then
    ACCEL="-accel kvm"
    echo "Using KVM acceleration"
else
    echo "WARNING: /dev/kvm not accessible — using software emulation (slower)"
    echo "  To enable nested KVM on WSL2, add to %USERPROFILE%\\.wslconfig:"
    echo "    [wsl2]"
    echo "    nestedVirtualization=true"
    echo "  Then: wsl --shutdown && restart WSL"
    echo ""
fi

MEM="${QEMU_MEM:-2G}"
SMP="${QEMU_SMP:-2}"

echo "VM: ${MEM} RAM, ${SMP} CPUs, repo shared at /mnt/src"
echo "Exit: Ctrl-A, X  |  QEMU monitor: Ctrl-A, C"
echo "───────────────────────────────────────────────"

exec qemu-system-x86_64 \
    $ACCEL \
    -m "$MEM" \
    -smp "$SMP" \
    -kernel "$VMDIR/vmlinuz" \
    -initrd "$VMDIR/initrd.img" \
    -append "root=/dev/vda rw console=ttyS0,115200 loglevel=6 crashkernel=256M" \
    -drive file="$VMDIR/disk.img",format=raw,if=virtio \
    -virtfs local,path="$REPO",mount_tag=xv6fs_src,security_model=none,id=xv6fs_src \
    -nographic
