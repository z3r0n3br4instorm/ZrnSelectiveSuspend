## 1. Patch revision 2

- [x] 1.1 Find the path that retried 341 times during a resume and make it wait for the resume instead of retrying at once (the page-fault path now waits 20 ms per retry; that it was the source of the 341 is an inference, to be confirmed on hardware)
- [x] 1.2 Add a pollable wake file under `/proc/driver/nvidia/`, woken on every wake request, using only symbols a non-GPL module may use
- [x] 1.3 Keep the counter parameter and document both in the patch header
- [x] 1.4 Regenerate `patches/nvidia-470.256.02-wake-on-touch.patch` and confirm it builds for the running kernel with no new warnings
- [x] 1.5 Add `patches/validated-versions` listing 470.256.02

## 2. Patch management tool

- [x] 2.1 Write `zss-nvidia-patch status`: driver version, validated or not, patch registered with DKMS or not, running module has wake support or not
- [x] 2.2 Write `apply`: refuse unvalidated versions, check the patch applies cleanly, copy it into the driver source, add marked `PATCH` lines to `dkms.conf`, keep the stock modules for the running kernel, rebuild and install through DKMS (the file changes are tested; the DKMS rebuild itself has not been run)
- [x] 2.3 Write `remove`: delete the marked lines and the patch file, rebuild and install the stock driver, and verify `dkms.conf` matches the packaged file
- [x] 2.4 Write the boot check unit: load the driver before the display manager, restore and load the stock modules if that fails, log the reason (written; first run is on the laptop)
- [x] 2.5 Write the pacman hook that runs `apply` after the driver package is installed or upgraded (written; first run is the next driver package update)
- [x] 2.6 Test `status`, `apply` and `remove` against a copy of the driver source in a temporary directory, including an unvalidated version and a patch that does not apply

## 3. Daemon: off and on

- [x] 3.1 Add the configuration file reader, with command-line options overriding it
- [x] 3.2 Add wake-support detection for a device's driver and show it in `zssctl status`
- [x] 3.3 Implement the holder rules: listed services, display server with wake support and no connected display, and refusal messages for everything else
- [x] 3.4 Detect whether the GPU is driving a connected display from its DRM connectors
- [x] 3.5 Implement power-off: migrate or park, stop services, unbind audio, save configuration to memory and `/run/zss/`, suspend, cut power, confirm, state `powered-off`
- [x] 3.6 Implement power-on: power, restore configuration, resume, rebind audio, start services, state `attached`, leaving migrated applications where they are
- [x] 3.7 Add `zssctl off`, `zssctl on` and `zssctl on --return`
- [x] 3.8 Test the sequence, every refusal, and a failed suspend against a fake backend and fake driver files

## 4. Daemon: wake requests

- [x] 4.1 Watch the wake file with `poll` while a device is off, falling back to reading the counter when the file is absent (the counter fallback is tested; waiting on the wake file needs the revision 2 driver loaded)
- [x] 4.2 Power on when a request arrives, and record the time and the count
- [x] 4.3 Test that a request powers the device on, that none arriving leaves it off, and that the fallback works

## 5. Daemon: idle timer

- [x] 5.1 Implement `idle_timeout`: power off after that long with no client and no connected display; never when unset
- [x] 5.2 Implement damping: double the wait after three power-offs each ended within a minute, cap at an hour, reset after one that lasts
- [x] 5.3 Show wake count and current wait in `zssctl status`
- [x] 5.4 Test the timer and the damping with a shortened clock

## 6. Service and recovery

- [x] 6.1 Write `zssd.service` and the default `/etc/zss/zssd.conf`
- [x] 6.2 Power every device on before a clean exit
- [x] 6.3 Implement `zssd --recover` and wire it as `ExecStopPost` (`--recover` is tested; the `ExecStopPost` wiring has not run under systemd)
- [ ] 6.4 Test under systemd: the service starts, group members may switch power and others may not, and a killed daemon leaves no device off (the QEMU guest has no systemd; permissions and recovery are covered by host tests, the unit itself is untested)

## 7. Installer and packaging

- [x] 7.1 Write the platform report and expose it as `install.sh --check`
- [x] 7.2 Write `install.sh`: binaries, launcher on the path, layer manifest, unit, configuration, group, with the driver step behind an explicit confirmation
- [x] 7.3 Make the installer leave bootloader configuration alone and regenerate the initial ramdisk only when the driver is in it, announcing it first
- [x] 7.4 Write `uninstall.sh`, reversing each step and keeping the configuration unless purging
- [x] 7.5 Write the Arch `PKGBUILD` and install script (written; not built with makepkg)
- [x] 7.6 Test `--check`, an install into a staging root, and that uninstalling leaves nothing behind

## 8. Reference laptop (each step needs the user's go-ahead)

- [ ] 8.1 Install through `install.sh`, replacing the hand-installed modules, and reboot
- [ ] 8.2 Confirm the driver loaded with wake support and that `dkms status` shows the patched build
- [ ] 8.3 Run `zssctl off`, a display query, and `zssctl on`; confirm no console switch and no freeze
- [ ] 8.4 Leave the device off under normal desktop use for ten minutes and record how often it is woken
- [ ] 8.5 Kill the daemon while the device is off and confirm the device comes back
- [ ] 8.6 Plug in an external display while the device is off and record what happens
- [ ] 8.7 Enable the idle timer for a working session and record wakes, time off, and battery draw

## 10. Progress reporting and the text console

- [x] 10.1 Report every step of a power-off and power-on, prefixed `[ZrnSelectiveSuspend]`, to the log and to the `zssctl` that asked
- [x] 10.2 Add `zssctl off --console`: switch to a text console, print the report there, stay while the device is off, return on power-on
- [x] 10.3 Power the device on when a key is pressed on that console (written; a key press cannot be injected in the tests)
- [x] 10.4 Test the report's content and order on the host, and the console switch, its contents and the return in the QEMU guest

## 11. Freezing what cannot be moved

- [x] 11.1 On a requested power-off, freeze processes outside the layer and layer applications that cannot be moved, instead of refusing; thaw them on every path that powers the device on
- [x] 11.2 Refuse to freeze the terminal the request came from (unless `--console`), and fail without cutting power if a process cannot be frozen
- [x] 11.3 Keep the idle timer from freezing anything: such a process counts as use
- [x] 11.4 Record frozen processes in the recovery marker and thaw them in `zssd --recover`
- [x] 11.5 Test on the host and, with a real freeze and a killed daemon, in the QEMU guest
- [ ] 11.6 On the laptop: `zssctl off` with `btop` running, confirm it is frozen, and that it carries on after `zssctl on`

## 12. A requested power-off stays off

- [x] 12.1 Patch revision 3: list the waiting processes in the wake file; confirm it builds for the running kernel
- [x] 12.2 Daemon: after a requested power-off, power on only for the display server or an unknown caller; log and count the others
- [x] 12.3 Name the caller in the wake reason when the driver gives it
- [x] 12.4 Test both rules against the fake wake file
- [ ] 12.5 On the laptop: install revision 3, reboot, `zssctl off`, run `nvidia-smi`, confirm the device stays off and the tool continues after `zssctl on`; record what, if anything, makes the display server wake it

## 13. Hiding a device that is off, and not getting stuck

- [x] 13.1 Never ask the driver anything from the daemon while the device is off (this froze the desktop once: `zssctl status` put the daemon to sleep in the driver)
- [x] 13.2 Service watchdog: a daemon that stops turning is killed and the recovery step restores the device
- [x] 13.3 Hide the device's nodes, its vendor driver's loader files and status files while it is off on request; undo on power-on, stop and recovery
- [x] 13.4 Test hiding, power-on and recovery after a kill in the QEMU guest
- [x] 13.5 On the laptop: with the device off, run Vulkan, OpenGL, EGL and NVIDIA tools; the device stays off and comes back complete (done 2026-10-06; see design D4b)
- [x] 13.7 Serve the display server and go off again: a requested off survives X calling the driver (on the laptop: closing a moved application, then a plain `vkcube` on Intel)
- [x] 13.8 Layer: give each driver its own X connection, closed when the driver is released
- [ ] 13.6 On the laptop: leave it off through a normal working session and record what, if anything, wakes it

## 9. Documents

- [x] 9.1 Document installation, the configuration file, `off`/`on`, and removal in the README
- [ ] 9.2 Record the hardware results and the monitor hot-plug limitation in `docs/track-c.md` (the limitation is recorded; the results wait for group 8)
- [x] 9.3 Update `docs/protocol.md` for the new requests and status fields
