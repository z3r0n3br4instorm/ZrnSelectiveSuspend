## Why

Powering the dGPU off under a running desktop now works on the reference laptop, but only by hand: a test script suspends the driver, cuts power, watches for a wake request and brings the card back, and the NVIDIA driver was patched and installed manually. `zssd` cannot do any of it: it treats Xorg and `nvidia-persistenced` as blockers, knows nothing about wake requests, and is not installed as a service. A kernel update would also silently replace the patched driver with the stock one, and X would freeze again the next time the card was switched off.

This change turns what was proven by hand into something that is installed, runs by itself, and keeps working across updates.

## What Changes

- `zssd` learns the suspend-in-place sequence that was run by hand: stop the holders that are idle by design, suspend the driver, cut power, and report the device as powered off while it stays attached to the display server.
- `zssd` watches for wake requests and powers the device on and resumes the driver when one arrives, without being asked.
- New commands `zssctl off` and `zssctl on`, and an optional idle timer that powers the device off after it has gone unused for a configured time.
- For a suspend-in-place device with a wake-capable driver, a display server holding the device no longer blocks powering it off. Applications rendering on it are still migrated first, and an application that cannot be migrated still blocks.
- The device is never powered off while it is driving a display.
- The NVIDIA patch becomes a managed thing: applied through DKMS so every rebuild includes it, verified at boot, re-applied after a driver package update, and removable. It is applied only to driver versions it has been validated against.
- The patch itself is tightened: callers that today retry in a tight loop while the driver resumes sleep instead, and wake requests can be waited for rather than polled.
- A systemd service, a configuration file and an administrators' group for `zssd`.
- An installer and an uninstaller, plus an Arch package definition, that check what the machine supports and install only what applies.

Not in this change: a power backend for laptops without an Apple gmux, support for NVIDIA driver versions other than those validated, detecting a monitor being plugged in while the device is off, and lifting the Vulkan 1.0 limit of the graphics layer.

## Capabilities

### New Capabilities

- `gpu-idle-power`: powering a suspend-in-place GPU off and on while the desktop keeps running, on request, on idle, and on a wake request.
- `nvidia-driver-patch`: installing, verifying, keeping and removing the wake-on-touch patch to the NVIDIA driver.
- `system-install`: installing ZSS on a machine as a service, with platform checks, and removing it again.

### Modified Capabilities

None. `zss-happy-path` and `zss-device-loss` are not archived, so there are no main specs to modify. Where this change relaxes a rule from `zss-happy-path` (a display server blocks every detach), the new requirement states the rule that applies to suspend-in-place devices in full.

## Impact

- **Code:** `src/daemon/` (suspend-in-place sequence, wake watcher, idle timer, holder handling, configuration file), `src/zssctl/`, `patches/`, new `packaging/` (systemd unit, DKMS and package-manager hooks, installer, Arch `PKGBUILD`), `tests/`, `docs/`.
- **System files on an installed machine:** a service unit, `/etc/zss/zssd.conf`, a `zss` group, a hook that re-applies the driver patch, and two lines added to the NVIDIA package's `dkms.conf`. The uninstaller removes all of them and rebuilds the stock driver.
- **The NVIDIA driver:** modified in its open kernel-interface layer only. The project ships a patch, never NVIDIA's files.
- **Risk:** a driver that fails to build or load after the patch leaves the machine without its discrete GPU until the patch is removed. The installer keeps the stock modules and a boot-time check restores them if the patched driver does not load.
- **Scope of "other computers":** after this change ZSS installs and runs anywhere, but can only power a GPU off on Apple laptops with a classic gmux and a validated NVIDIA driver. Elsewhere the installer says so and installs migration only.
