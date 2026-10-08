#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Removes ZrnSelectiveSuspend: powers on any GPU that is off, stops the
# service, rebuilds the stock GPU driver, and deletes what install.sh added.
#
# Options: --destdir DIR   remove from a staging root (files only)
#          --purge         also remove /etc/zss, the zss group and the kept stock modules
set -u

DESTDIR=""; PREFIX=/usr/local; PURGE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --destdir) DESTDIR="$2"; shift 2 ;;
        --purge) PURGE=1; shift ;;
        *) sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
D="$DESTDIR"
say() { echo "$*"; }

if [ -z "$DESTDIR" ]; then
    [ "$(id -u)" = 0 ] || { echo "uninstall.sh: needs root" >&2; exit 1; }
    # Stopping the service powers every device back on (and --recover covers a daemon that is already dead).
    systemctl disable --now zssd.service >/dev/null 2>&1
    [ -x "$PREFIX/sbin/zssd" ] && "$PREFIX/sbin/zssd" --recover >/dev/null 2>&1
    if [ -x "$PREFIX/sbin/zss-nvidia-patch" ]; then
        ZSS_PATCH_DIR="$PREFIX/share/zss/patches" "$PREFIX/sbin/zss-nvidia-patch" remove
    fi
    systemctl disable zss-nvidia-check.service >/dev/null 2>&1
    # The module restores every device it holds as it unloads.
    [ -d /sys/kernel/zss ] && rmmod zss
    for src in /usr/src/zss-[0-9]*; do
        [ -f "$src/dkms.conf" ] && dkms remove "zss/${src#/usr/src/zss-}" --all >/dev/null 2>&1
    done
fi
rm -rf "$D"/usr/src/zss-[0-9]*

rm -f "$D$PREFIX/sbin/zssd" "$D$PREFIX/sbin/zss-nvidia-patch" "$D$PREFIX/sbin/zss-power-event" "$D$PREFIX/bin/zssctl" "$D$PREFIX/bin/zss-run" \
      "$D/etc/systemd/system/zssd.service" "$D/etc/systemd/system/zss-nvidia-check.service" \
      "$D/etc/pacman.d/hooks/65-zss-nvidia-patch.hook"
rm -rf "$D$PREFIX/lib/zss" "$D$PREFIX/share/zss" "$D$PREFIX/share/doc/zss"
if [ "$PURGE" = 1 ]; then
    rm -rf "$D/etc/zss"
    if [ -z "$DESTDIR" ]; then
        rm -rf /var/lib/zss
        getent group zss >/dev/null && groupdel zss
    fi
else
    [ -d "$D/etc/zss" ] && say "kept $D/etc/zss (use --purge to remove it)"
fi
[ -z "$DESTDIR" ] && systemctl daemon-reload
say "ZrnSelectiveSuspend removed"
