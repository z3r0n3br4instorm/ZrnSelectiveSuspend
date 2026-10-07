## Purpose

Gets the kernel module built, installed and loaded without putting the machine's ability to boot at risk, and takes it off again cleanly.

## ADDED Requirements

### Requirement: The module is built through DKMS
The module SHALL be registered with DKMS so that it is rebuilt for each installed kernel.

#### Scenario: Kernel update
- **WHEN** a new kernel is installed
- **THEN** the module is built for it without user action

#### Scenario: Build failure
- **WHEN** the module does not build for a kernel
- **THEN** the failure is reported and the daemon runs without the module on that kernel

### Requirement: Installing the module is optional and confirmed
The installer SHALL install the module only after an explicit yes, and everything else SHALL work without it.

#### Scenario: User declines
- **WHEN** the user declines the kernel module
- **THEN** the rest of ZSS is installed and works as before

### Requirement: The module cannot stop the machine from booting
The module SHALL NOT be included in the initial ramdisk and SHALL NOT be loaded before the daemon's service starts.

#### Scenario: Boot
- **WHEN** the machine boots with the module installed
- **THEN** the module is loaded only when the daemon's service starts

### Requirement: Removal restores the machine
Uninstalling SHALL unload the module, which restores every managed device, and SHALL remove it from DKMS.

#### Scenario: Uninstall
- **WHEN** ZSS is uninstalled
- **THEN** the module is not loaded, is not registered with DKMS, and every GPU is powered
