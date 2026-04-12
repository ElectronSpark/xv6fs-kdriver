#!/bin/bash
# vm/setup.sh - Create a Debian VM for xv6fs kernel module development
#
# Creates a QEMU/KVM-ready VM with:
#   - Stock Debian kernel + headers (module builds work out of the box)
#   - 9p share: repo mounted at /mnt/src inside the guest
#   - kdump enabled (crash dumps saved to /var/crash/)
#   - Persistent journald logging at info level
#   - Serial console with root autologin
#
# Usage: ./vm/setup.sh [disk-size]
#   disk-size defaults to 4G
#
# Prerequisites: sudo apt install debootstrap qemu-system-x86 qemu-utils

set -euo pipefail

VMDIR="$(cd "$(dirname "$0")" && pwd)"
DISK="$VMDIR/disk.img"
DISK_SIZE="${1:-4G}"

# ── Check prerequisites ──────────────────────────────────────────────
missing=()
for cmd in debootstrap qemu-system-x86_64 qemu-img; do
    command -v "$cmd" &>/dev/null || missing+=("$cmd")
done
if (( ${#missing[@]} )); then
    echo "ERROR: Missing commands: ${missing[*]}"
    echo "Install: sudo apt install debootstrap qemu-system-x86 qemu-utils"
    exit 1
fi

if [[ -f "$DISK" ]]; then
    echo "ERROR: $DISK already exists. Remove it first to recreate."
    exit 1
fi

# ── Temp mountpoint + cleanup trap ───────────────────────────────────
MNT=$(mktemp -d)
cleanup() {
    echo "==> Cleaning up mounts..."
    for mp in dev/pts dev sys proc; do
        sudo umount "$MNT/$mp" 2>/dev/null || true
    done
    sudo umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

# ── Create and mount disk image ──────────────────────────────────────
echo "==> Creating ${DISK_SIZE} raw disk image..."
qemu-img create -f raw "$DISK" "$DISK_SIZE"
mkfs.ext4 -q -F "$DISK"
sudo mount -o loop "$DISK" "$MNT"

# ── Bootstrap Debian bookworm ────────────────────────────────────────
PACKAGES="linux-image-amd64,linux-headers-amd64"
PACKAGES+=",build-essential,dwarves,bc"
PACKAGES+=",systemd,systemd-sysv,dbus"
PACKAGES+=",kexec-tools,kdump-tools,makedumpfile"
PACKAGES+=",kmod,vim-tiny,less,bash-completion"

echo "==> Bootstrapping Debian trixie (this takes several minutes)..."
sudo debootstrap --include="$PACKAGES" \
    trixie "$MNT" http://deb.debian.org/debian

# ── Mount pseudo-filesystems for chroot ──────────────────────────────
echo "==> Configuring guest system..."
sudo mount --bind /proc "$MNT/proc"
sudo mount --bind /sys  "$MNT/sys"
sudo mount --bind /dev  "$MNT/dev"
sudo mount --bind /dev/pts "$MNT/dev/pts"

# ── Hostname ─────────────────────────────────────────────────────────
echo "xv6fs-dev" | sudo tee "$MNT/etc/hostname" > /dev/null

# ── fstab: root filesystem + 9p share ────────────────────────────────
sudo tee "$MNT/etc/fstab" > /dev/null <<'EOF'
/dev/vda    /           ext4    errors=remount-ro                                           0 1
xv6fs_src   /mnt/src    9p      trans=virtio,version=9p2000.L,msize=104857600,nofail,_netdev 0 0
EOF
sudo mkdir -p "$MNT/mnt/src"

# ── Initramfs: ensure virtio + 9p modules are included ───────────────
sudo tee "$MNT/etc/initramfs-tools/modules" > /dev/null <<'EOF'
virtio_blk
virtio_pci
virtio_net
9p
9pnet
9pnet_virtio
ext4
EOF

KVER=$(ls "$MNT/lib/modules/" | grep -v '^$' | sort -V | tail -1)
echo "    Guest kernel: $KVER"
echo "==> Regenerating initramfs for $KVER..."
sudo chroot "$MNT" update-initramfs -u -k "$KVER"

# ── Serial console: root autologin ───────────────────────────────────
sudo mkdir -p "$MNT/etc/systemd/system/serial-getty@ttyS0.service.d"
sudo tee "$MNT/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" > /dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I 115200 linux
EOF
sudo chroot "$MNT" systemctl enable serial-getty@ttyS0.service 2>/dev/null || true

# ── Persistent journal at info level ─────────────────────────────────
sudo mkdir -p "$MNT/var/log/journal"
sudo mkdir -p "$MNT/etc/systemd/journald.conf.d"
sudo tee "$MNT/etc/systemd/journald.conf.d/persistent.conf" > /dev/null <<'EOF'
[Journal]
Storage=persistent
MaxLevelStore=info
SystemMaxUse=200M
EOF

# ── kdump configuration ─────────────────────────────────────────────
sudo tee "$MNT/etc/default/kdump-tools" > /dev/null <<'EOF'
USE_KDUMP=1
KDUMP_SYSCTL="kernel.panic_on_oops=1"
KDUMP_COREDIR="/var/crash"
EOF
sudo mkdir -p "$MNT/var/crash"

# ── Root password: blank (dev VM only) ───────────────────────────────
sudo chroot "$MNT" passwd -d root 2>/dev/null

# ── DNS (for any post-setup apt installs inside VM) ──────────────────
sudo cp /etc/resolv.conf "$MNT/etc/resolv.conf" 2>/dev/null || true

# ── Helper scripts ───────────────────────────────────────────────────
sudo tee "$MNT/usr/local/bin/xv6fs-reload" > /dev/null <<'SCRIPT'
#!/bin/bash
set -e
cd /mnt/src
umount /mnt/test 2>/dev/null || true
rmmod xv6fs 2>/dev/null || true
make clean && make
insmod xv6fs.ko
echo "xv6fs module loaded"
SCRIPT
sudo chmod +x "$MNT/usr/local/bin/xv6fs-reload"

sudo tee "$MNT/usr/local/bin/xv6fs-test" > /dev/null <<'SCRIPT'
#!/bin/bash
set -e
mkdir -p /mnt/test
mountpoint -q /mnt/test && umount /mnt/test
mount -t xv6fs -o loop /mnt/src/test.img /mnt/test
echo "Mounted at /mnt/test:"
ls -la /mnt/test
SCRIPT
sudo chmod +x "$MNT/usr/local/bin/xv6fs-test"

# ── Root shell convenience ───────────────────────────────────────────
sudo tee -a "$MNT/root/.bashrc" > /dev/null <<'BASH'

# ── xv6fs development helpers ──
alias reload='xv6fs-reload'
alias testmount='xv6fs-test'
alias testumount='umount /mnt/test 2>/dev/null; echo unmounted'
alias klog='journalctl -k --no-pager | tail -50'
alias prevboot='journalctl -b -1 --no-pager'
alias lastcrash='ls -lt /var/crash/ 2>/dev/null | head -10'
export PS1='xv6fs-dev:\w# '
cd /mnt/src 2>/dev/null || true
BASH

# ── Extract kernel + initrd for QEMU -kernel boot ───────────────────
echo "==> Extracting kernel and initrd..."
sudo cp "$MNT/boot/vmlinuz-$KVER"    "$VMDIR/vmlinuz"
sudo cp "$MNT/boot/initrd.img-$KVER" "$VMDIR/initrd.img"
sudo chown "$(id -u):$(id -g)" "$VMDIR/vmlinuz" "$VMDIR/initrd.img"

# ── Copy kernel headers for host IntelliSense ───────────────────────
#
# Debian splits headers into arch-specific + common + kbuild.  The
# arch-specific tree contains symlinks into the other two, so we
# dereference (-L) when copying to make a self-contained tree.
echo "==> Extracting kernel headers for host IntelliSense..."
KHEADERS="$VMDIR/kheaders"
rm -rf "$KHEADERS"
mkdir -p "$KHEADERS"

ARCH_HDR="$MNT/usr/src/linux-headers-$KVER"
COMMON_HDR="$MNT/usr/src/linux-headers-${KVER%-*}-common"

sudo cp -aL "$ARCH_HDR"   "$KHEADERS/amd64"
sudo cp -aL "$COMMON_HDR" "$KHEADERS/common"
sudo chown -R "$(id -u):$(id -g)" "$KHEADERS"

# Fix the arch Makefile to use the local common tree instead of /usr/src
sed -i "s|/usr/src/linux-headers-[^ ]*/|$KHEADERS/common/|g" \
    "$KHEADERS/amd64/Makefile"

echo "    Headers: $KHEADERS/{amd64,common}"

# ── Unmount ──────────────────────────────────────────────────────────
echo "==> Unmounting..."
for mp in dev/pts dev sys proc; do
    sudo umount "$MNT/$mp" 2>/dev/null || true
done
sudo umount "$MNT"
trap - EXIT
rmdir "$MNT"

echo ""
echo "================================================"
echo "  VM setup complete!"
echo ""
echo "  Disk:   $DISK"
echo "  Kernel: $VMDIR/vmlinuz ($KVER)"
echo "  Initrd: $VMDIR/initrd.img"
echo ""
echo "  Launch:  ./vm/run.sh"
echo ""
echo "  Inside the VM:"
echo "    reload       rebuild + insmod xv6fs.ko"
echo "    testmount    mount test.img at /mnt/test"
echo "    testumount   unmount /mnt/test"
echo "    klog         recent kernel log"
echo "    prevboot     previous boot's full journal"
echo "    lastcrash    list crash dumps in /var/crash/"
echo ""
echo "  Exit VM:  Ctrl-A, X"
echo "================================================"
