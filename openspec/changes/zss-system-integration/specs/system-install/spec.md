## Purpose

Gets ZSS onto a machine as a working service with one command, installs only what that machine can use, and takes it off again cleanly.

## ADDED Requirements

### Requirement: The installer reports what the machine supports
Before changing anything, the installer SHALL determine and print which parts of ZSS apply to the machine: application migration, power-off through a supported backend, and wake support in the GPU driver.

#### Scenario: Reference laptop
- **WHEN** the installer runs on an Apple laptop with a classic gmux and a validated NVIDIA driver
- **THEN** it reports migration, power-off and wake support as available

#### Scenario: Machine without a supported power backend
- **WHEN** the installer runs on a machine with no supported power backend
- **THEN** it reports that only application migration is available, and does not modify any driver

#### Scenario: Dry run
- **WHEN** the installer is run in its report-only mode
- **THEN** it prints the report and changes nothing

### Requirement: Installation is confirmed before the driver is changed
The installer SHALL ask for confirmation before modifying the GPU driver, stating that a reboot is needed and how to undo the change. A non-interactive mode SHALL require an explicit option to allow it.

#### Scenario: User declines
- **WHEN** the user declines the driver change
- **THEN** everything else is installed and the driver is untouched

### Requirement: The daemon runs as a service
Installation SHALL set up the daemon as a system service that starts at boot, reads its settings from a configuration file, and manages the GPUs the installer found or the file names.

#### Scenario: After installation
- **WHEN** installation completes and the machine has been rebooted
- **THEN** the daemon is running and `zssctl status` lists the managed GPU

#### Scenario: Configuration change
- **WHEN** the administrator changes the idle timeout in the configuration file and restarts the service
- **THEN** the new timeout is in effect

### Requirement: Access is limited to administrators
Installation SHALL create a group whose members may power GPUs off and on, detach and attach them. The installing user SHALL be offered membership.

#### Scenario: Member of the group
- **WHEN** a member of the group runs `zssctl off`
- **THEN** the request is accepted

#### Scenario: Other user
- **WHEN** a user outside the group runs `zssctl off`
- **THEN** the request is rejected with a permission error

### Requirement: Applications can be started under the layer by name
Installation SHALL put the launcher on the command path so that an application can be started under the ZSS layer without referring to the build directory.

#### Scenario: Launcher
- **WHEN** the user runs the launcher with an application's command line
- **THEN** the application starts under the ZSS layer and is listed by `zssctl status`

### Requirement: Bootloader and initial ramdisk are not modified
Installation and removal SHALL NOT modify the bootloader's configuration. They SHALL regenerate the initial ramdisk only if the GPU driver is part of it, and SHALL say so before doing it.

#### Scenario: Driver not in the initial ramdisk
- **WHEN** ZSS is installed on a machine whose initial ramdisk does not contain the GPU driver
- **THEN** neither the bootloader configuration nor the initial ramdisk is changed

### Requirement: Removal restores the machine
The uninstaller SHALL power on any GPU that is off, stop and remove the service, remove the driver patch as its own requirement describes, and remove the files and hooks the installer added. It SHALL leave the configuration file in place unless asked to purge it.

#### Scenario: Uninstall
- **WHEN** ZSS is uninstalled
- **THEN** the service no longer exists, the GPU driver is the stock one, and no ZSS hook remains
