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
#          --kernel-module      build and install the zss kernel module without asking
#          --no-kernel-module   do not install the kernel module
#          --no-start           install everything but leave the service as it is
#                               (for when the next step is a reboot anyway)
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
KMOD=ask
START=yes
KMOD_VERSION="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$REPO/kmod/dkms.conf")"

while [ $# -gt 0 ]; do
    case "$1" in
        --check) CHECK=1; shift ;;
        --build) BUILD="$2"; shift 2 ;;
        --destdir) DESTDIR="$2"; shift 2 ;;
        --patch-driver) PATCH=yes; shift ;;
        --no-patch-driver) PATCH=no; shift ;;
        --kernel-module) KMOD=yes; shift ;;
        --no-kernel-module) KMOD=no; shift ;;
        --no-start) START=no; shift ;;
        *) sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done

say() { echo "$*"; }
die() { echo "install.sh: $*" >&2; exit 1; }

# ---- what does this machine support? -------------------------------------------------

GPU=""; GPU_NAME=""; BACKEND="none"; DRIVER=""; DRIVER_VERSION=""; VALIDATED=no; IN_INITRD=no; IN_FALLBACK=no

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
    # Boot images are plain initramfs files on some systems and unified kernel
    # images on others. A fallback image holds every driver by design and is
    # reported apart: it is not what the machine normally boots.
    if command -v lsinitcpio >/dev/null 2>&1; then
        local img seen=0
        for img in /boot/initramfs-*.img /boot/EFI/Linux/*.efi /efi/EFI/Linux/*.efi /boot/efi/EFI/Linux/*.efi; do
            [ -e "$img" ] || continue
            [ -r "$img" ] || { IN_INITRD="unknown (boot images are readable by root only)"; continue; }
            seen=1
            if lsinitcpio "$img" 2>/dev/null | grep -q 'nvidia.*\.ko'; then
                case "$img" in *fallback*) IN_FALLBACK=yes ;; *) IN_INITRD=yes ;; esac
            fi
        done
        [ "$seen" = 0 ] && [ "$IN_INITRD" = no ] && [ "$(id -u)" != 0 ] && IN_INITRD="unknown (run as root to look)"
    fi
}

report() {
    say "ZrnSelectiveSuspend: what this machine supports"
    say ""
    say "  Application migration       yes (Vulkan 1.1 applications started with zss-run)"
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
    [ "$IN_FALLBACK" = yes ] && say "  Driver in fallback image    yes: booting the fallback entry loads that older copy, without the patch"
    if [ -d /sys/kernel/zss ]; then
        say "  ZSS kernel module           loaded (version $(cat /sys/kernel/zss/version 2>/dev/null))"
    elif [ -d "/usr/src/zss-$KMOD_VERSION" ]; then
        say "  ZSS kernel module           installed, not loaded"
    elif [ -f "/usr/lib/modules/$(uname -r)/build/Makefile" ] && command -v dkms >/dev/null 2>&1; then
        say "  ZSS kernel module           not installed (can be built: kernel headers and DKMS are present)"
    else
        say "  ZSS kernel module           not installed (needs kernel headers and DKMS to build)"
    fi
    if [ -n "$GPU" ] && [ -e "/sys/bus/pci/devices/$GPU/iommu_group" ]; then
        say "  IOMMU                       on: the GPU's memory access is confined by the kernel"
    else
        say "  IOMMU                       off: nothing confines the GPU's memory access"
    fi
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
# One launcher for the build tree and the installed system; only the manifest's path differs.
sed "s|@MANIFEST@|$PREFIX/share/zss/zss_icd.json|" "$REPO/src/layer/zss-run.in" > "$D$PREFIX/bin/zss-run"
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

# The kernel module's source goes where DKMS looks for it. Nothing is built or loaded here.
kmod_source() {
    install -d "$D/usr/src/zss-$KMOD_VERSION" || return 1
    install -m 644 "$REPO/kmod/zss.c" "$REPO/kmod/Kbuild" "$REPO/kmod/Makefile" "$REPO/kmod/dkms.conf" \
            "$D/usr/src/zss-$KMOD_VERSION/"
}

if [ -n "$DESTDIR" ]; then
    [ "$KMOD" = yes ] && kmod_source
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

# ---- the kernel module ---------------------------------------------------------------

if [ "$KMOD" = ask ]; then
    if [ ! -f "/usr/lib/modules/$(uname -r)/build/Makefile" ] || ! command -v dkms >/dev/null 2>&1; then
        KMOD=no
    else
        say ""
        say "The zss kernel module does the power sequence inside the kernel, for any GPU driver."
        say "  - It is built through DKMS and rebuilt for each new kernel."
        say "  - It is loaded when the zssd service starts, never from the initial ramdisk."
        say "  - Loaded, it does nothing until the daemon hands it a device."
        say "  - Without it everything works as before. Undo with: packaging/uninstall.sh, or rmmod zss"
        printf "Install the kernel module? [y/N] "
        read -r answer
        case "$answer" in y|Y|yes) KMOD=yes ;; *) KMOD=no ;; esac
    fi
fi
if [ "$KMOD" = yes ]; then
    # The daemon powers every device on as it stops; only then can the old module go.
    systemctl stop zssd.service >/dev/null 2>&1
    if [ -d /sys/kernel/zss ] && ! rmmod zss; then
        die "the loaded zss module could not be removed; the rest is installed"
    fi
    dkms remove "zss/$KMOD_VERSION" --all >/dev/null 2>&1
    kmod_source || die "cannot write /usr/src/zss-$KMOD_VERSION"
    if dkms add "zss/$KMOD_VERSION" >/dev/null 2>&1 && dkms build "zss/$KMOD_VERSION" >/dev/null 2>&1 &&
       dkms install "zss/$KMOD_VERSION" --force >/dev/null 2>&1; then
        say "kernel module zss $KMOD_VERSION built and installed through DKMS"
    else
        say "warning: the kernel module did not build (see: dkms status zss); ZSS runs without it"
    fi
else
    [ -d "/usr/src/zss-$KMOD_VERSION" ] && say "the kernel module was left as it is" || say "the kernel module was not installed"
fi

systemctl daemon-reload
if [ "$START" = no ]; then
    systemctl enable zssd.service >/dev/null 2>&1
    say "the service was not started; it starts at the next boot"
elif grep -q '^gpu = ' /etc/zss/zssd.conf; then
    # Restart, not just start: on a reinstall the running daemon is the old build.
    systemctl enable zssd.service >/dev/null 2>&1
    systemctl restart zssd.service && say "zssd is running"
else
    say "no GPU is configured in /etc/zss/zssd.conf; set one and run: systemctl enable --now zssd"
fi
say "done. Try: zssctl status"
