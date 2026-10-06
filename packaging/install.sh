#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Installs ZrnSelectiveSuspend from a finished build.
#
#   ./packaging/install.sh --check          report what this machine supports; change nothing
#   sudo ./packaging/install.sh             install; asks before touching the GPU driver
#
# Options: --build DIR          the Meson build directory (default ./build)
#          --destdir DIR        install into a staging root; skips everything that
#                               acts on the running system (group, services, driver)
#          --patch-driver       apply the driver patch without asking
#          --no-patch-driver    never touch the driver
#
# The bootloader's configuration is never modified. The initial ramdisk is
# regenerated only if the GPU driver is part of it, and that is announced first.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
BUILD="$REPO/build"
DESTDIR=""
PREFIX=/usr/local
CHECK=0
PATCH=ask

while [ $# -gt 0 ]; do
    case "$1" in
        --check) CHECK=1; shift ;;
        --build) BUILD="$2"; shift 2 ;;
        --destdir) DESTDIR="$2"; shift 2 ;;
        --patch-driver) PATCH=yes; shift ;;
        --no-patch-driver) PATCH=no; shift ;;
        *) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done

say() { echo "$*"; }
die() { echo "install.sh: $*" >&2; exit 1; }

# ---- what does this machine support? -------------------------------------------------

GPU=""; GPU_NAME=""; BACKEND="none"; DRIVER=""; DRIVER_VERSION=""; VALIDATED=no; IN_INITRD=no

detect() {
    local d class vendor
    for d in /sys/bus/pci/devices/*; do
        class="$(cat "$d/class" 2>/dev/null)"; vendor="$(cat "$d/vendor" 2>/dev/null)"
        case "$class" in 0x03*) ;; *) continue ;; esac
        # The integrated GPU drives the panel; the one to manage is the other one.
        [ "$vendor" = "0x8086" ] && continue
        GPU="$(basename "$d")"
        DRIVER="$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)"
    done
    [ -n "$GPU" ] && GPU_NAME="$(lspci -s "$GPU" 2>/dev/null | sed 's/^[^:]*:[^:]*: //')"
    if grep -qi '^APP000B$' /sys/bus/pnp/devices/*/id 2>/dev/null; then
        BACKEND="apple-gmux"
    elif [ -n "$GPU" ] && grep -qs "^${GPU%.*}\$" /sys/bus/pci/slots/*/address 2>/dev/null; then
        BACKEND="pciehp-slot"
    fi
    local src
    for src in /usr/src/nvidia-[0-9]*; do
        [ -f "$src/dkms.conf" ] && DRIVER_VERSION="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$src/dkms.conf" | head -1)"
    done
    [ -n "$DRIVER_VERSION" ] && grep -qxF "$DRIVER_VERSION" "$REPO/patches/validated-versions" && VALIDATED=yes
    if command -v lsinitcpio >/dev/null 2>&1; then
        local img
        for img in /boot/initramfs-*.img; do
            [ -r "$img" ] && lsinitcpio "$img" 2>/dev/null | grep -q 'nvidia.*\.ko' && IN_INITRD=yes
        done
    fi
}

report() {
    say "ZrnSelectiveSuspend: what this machine supports"
    say ""
    say "  Application migration       yes (Vulkan 1.0 applications started with zss-run)"
    if [ -z "$GPU" ]; then
        say "  Discrete GPU                none found"
    else
        say "  Discrete GPU                $GPU  $GPU_NAME"
        say "  Kernel driver               ${DRIVER:-none bound}"
    fi
    case "$BACKEND" in
        apple-gmux)  say "  Power-off                   yes, through the Apple gmux (the GPU is soldered: no removal)" ;;
        pciehp-slot) say "  Power-off                   yes, through the hot-plug slot (the GPU can be removed)" ;;
        *)           say "  Power-off                   no: no supported power backend on this machine" ;;
    esac
    if [ -z "$DRIVER_VERSION" ]; then
        say "  Wake on demand              no: no NVIDIA DKMS driver found"
    elif [ "$VALIDATED" = yes ]; then
        say "  Wake on demand              available: NVIDIA $DRIVER_VERSION is validated for the driver patch"
    else
        say "  Wake on demand              no: NVIDIA $DRIVER_VERSION has not been validated; the driver will be left alone"
    fi
    if [ -e /proc/driver/nvidia/zss_wake ] || [ -e /sys/module/nvidia/parameters/zss_wake_requests ]; then
        say "  Running driver              already has wake support"
    fi
    say "  Driver in initial ramdisk   $IN_INITRD"
    say ""
    if [ "$BACKEND" = none ]; then
        say "Only application migration will be installed. The GPU driver will not be modified."
    elif [ "$VALIDATED" != yes ]; then
        say "Power-off will work only from a text console or with nothing holding the GPU:"
        say "without wake support, a display server waiting on the GPU would freeze."
    else
        say "With the driver patch the GPU can be powered off under the running desktop."
    fi
}

detect
report
[ "$CHECK" = 1 ] && exit 0

# ---- install ---------------------------------------------------------------------

[ -x "$BUILD/src/daemon/zssd" ] || die "no build in $BUILD; run: meson setup build && ninja -C build"
if [ -z "$DESTDIR" ] && [ "$(id -u)" != 0 ]; then
    die "installing needs root (or use --destdir for a staging root)"
fi

D="$DESTDIR"
install -d "$D$PREFIX/bin" "$D$PREFIX/sbin" "$D$PREFIX/lib/zss" "$D$PREFIX/share/zss/patches" \
           "$D$PREFIX/share/doc/zss" "$D/etc/zss" "$D/etc/systemd/system" "$D/etc/pacman.d/hooks" || die "cannot create directories"
install -m 755 "$BUILD/src/daemon/zssd" "$D$PREFIX/sbin/zssd"
install -m 755 "$BUILD/src/zssctl/zssctl" "$D$PREFIX/bin/zssctl"
install -m 755 "$BUILD/src/layer/libzss_vk.so" "$D$PREFIX/lib/zss/libzss_vk.so"
install -m 755 "$HERE/zss-nvidia-patch" "$D$PREFIX/sbin/zss-nvidia-patch"
install -m 644 "$REPO"/patches/* "$D$PREFIX/share/zss/patches/"
install -m 644 "$REPO/README.md" "$D$PREFIX/share/doc/zss/README.md"
install -m 644 "$HERE/zssd.service" "$HERE/zss-nvidia-check.service" "$D/etc/systemd/system/"

# The manifest lives outside the loader's search path on purpose: only
# applications started through zss-run get the layer.
cat > "$D$PREFIX/share/zss/zss_icd.json" <<JSON
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": "$PREFIX/lib/zss/libzss_vk.so",
        "api_version": "1.0.0"
    }
}
JSON
cat > "$D$PREFIX/bin/zss-run" <<RUN
#!/bin/sh
# Runs a program with the ZSS graphics layer as its only Vulkan driver, which
# is what makes the program migratable. Usage: zss-run <program> [args...]
manifest="\${ZSS_MANIFEST:-$PREFIX/share/zss/zss_icd.json}"
if [ \$# -eq 0 ]; then
    echo "usage: zss-run <program> [args...]" >&2
    exit 2
fi
export VK_DRIVER_FILES="\$manifest"
export VK_ICD_FILENAMES="\$manifest"
exec "\$@"
RUN
chmod 755 "$D$PREFIX/bin/zss-run"

if [ -f "$D/etc/zss/zssd.conf" ]; then
    say "keeping the existing /etc/zss/zssd.conf"
else
    install -m 644 "$HERE/zssd.conf" "$D/etc/zss/zssd.conf"
    if [ -n "$GPU" ] && [ "$BACKEND" != none ]; then
        sed -i "s|^#gpu = .*|gpu = $GPU|" "$D/etc/zss/zssd.conf"
    fi
fi
say "installed files under $D$PREFIX and $D/etc/zss"

if [ -n "$DESTDIR" ]; then
    install -m 644 "$HERE/65-zss-nvidia-patch.hook" "$D/etc/pacman.d/hooks/"
    say "staging root: the group, the services and the driver were not touched"
    exit 0
fi

getent group zss >/dev/null || groupadd --system zss
if [ -n "${SUDO_USER:-}" ] && ! id -nG "$SUDO_USER" | grep -qw zss; then
    usermod -aG zss "$SUDO_USER" && say "added $SUDO_USER to the zss group (log in again for it to take effect)"
fi

# ---- the driver ------------------------------------------------------------------

if [ "$BACKEND" != none ] && [ "$VALIDATED" = yes ] && [ "$PATCH" != no ]; then
    if [ "$PATCH" = ask ]; then
        say ""
        say "The NVIDIA $DRIVER_VERSION driver can be patched so the GPU wakes on demand."
        say "  - The driver is rebuilt through DKMS; a reboot is needed for it to load."
        say "  - The stock modules are kept, and restored at boot if the patched driver does not load."
        say "  - Undo at any time with: zss-nvidia-patch remove"
        [ "$IN_INITRD" = yes ] && say "  - The driver is in the initial ramdisk, which will be regenerated."
        printf "Patch the driver now? [y/N] "
        read -r answer
        case "$answer" in y|Y|yes) PATCH=yes ;; *) PATCH=no ;; esac
    fi
    if [ "$PATCH" = yes ]; then
        install -m 644 "$HERE/65-zss-nvidia-patch.hook" /etc/pacman.d/hooks/ 2>/dev/null
        ZSS_PATCH_DIR="$PREFIX/share/zss/patches" "$PREFIX/sbin/zss-nvidia-patch" apply || die "the driver patch failed; the rest is installed"
        systemctl enable zss-nvidia-check.service >/dev/null 2>&1
        if [ "$IN_INITRD" = yes ]; then
            say "regenerating the initial ramdisk because the NVIDIA driver is part of it"
            mkinitcpio -P || say "warning: mkinitcpio failed; run it by hand before rebooting"
        fi
        say "driver patched: reboot for it to take effect"
    else
        say "the driver was left as it is"
    fi
fi

systemctl daemon-reload
if grep -q '^gpu = ' /etc/zss/zssd.conf; then
    # Restart, not just start: on a reinstall the running daemon is the old build.
    systemctl enable zssd.service >/dev/null 2>&1
    systemctl restart zssd.service && say "zssd is running"
else
    say "no GPU is configured in /etc/zss/zssd.conf; set one and run: systemctl enable --now zssd"
fi
say "done. Try: zssctl status"
