## Purpose

Keeps the NVIDIA driver able to signal a waiting caller, across kernel and driver updates, without leaving a machine with a driver that does not load.

## ADDED Requirements

### Requirement: The patch changes nothing while the driver runs
With the patch applied and the driver not suspended, every call into the driver SHALL behave as it does without the patch.

#### Scenario: Normal use
- **WHEN** applications use the patched driver while it is not suspended
- **THEN** no wake request is recorded

### Requirement: A waiting caller is signalled and sleeps
With the patch applied and the driver suspended, a call into the driver SHALL record a wake request that user space can wait for, and SHALL sleep until the driver is resumed. No caller SHALL spin while waiting.

#### Scenario: Call while suspended
- **WHEN** a process calls into the suspended driver
- **THEN** a wake request is recorded and the process uses no processor time until the driver is resumed

#### Scenario: Resume
- **WHEN** the driver is resumed while callers are waiting
- **THEN** every waiting caller continues and completes

#### Scenario: Who is waiting
- **WHEN** processes are waiting on the suspended driver
- **THEN** user space can read the process id of each of them

#### Scenario: Waiting caller is killed
- **WHEN** a process waiting on the suspended driver receives a fatal signal
- **THEN** the process exits

### Requirement: The patch is applied only to validated driver versions
The system SHALL apply the patch only to driver versions listed as validated. On any other version it SHALL leave the driver untouched and say which version it found.

#### Scenario: Unknown driver version
- **WHEN** the installer finds an NVIDIA driver version that is not on the validated list
- **THEN** the driver is not modified and the installer reports that power-off under a display server is unavailable

### Requirement: The patch survives rebuilds and updates
Once installed, the patch SHALL be included whenever the driver is rebuilt for a new kernel, and SHALL be re-applied when the driver package is updated to a validated version.

#### Scenario: Kernel update
- **WHEN** a new kernel is installed and the driver is rebuilt for it
- **THEN** the rebuilt driver includes the patch

#### Scenario: Driver package updated to an unvalidated version
- **WHEN** the driver package is updated to a version that is not validated
- **THEN** the patch is not applied, and the daemon reports that the driver lacks wake support

### Requirement: A driver that does not load is rolled back
The stock driver modules SHALL be kept when the patched ones are installed. If the patched driver fails to load at boot, the system SHALL restore the stock modules and load them before the display server starts.

#### Scenario: Patched driver fails to load
- **WHEN** the patched driver cannot be loaded at boot
- **THEN** the stock modules are restored and loaded, and the failure is logged

### Requirement: The patch state is visible
The system SHALL report whether the running driver has wake support.

#### Scenario: Status
- **WHEN** the user asks for the status of a GPU
- **THEN** the output says whether its driver has wake support

### Requirement: The patch can be removed
The system SHALL provide a way to remove the patch that leaves the stock driver built, installed and registered as before.

#### Scenario: Removal
- **WHEN** the patch is removed
- **THEN** the driver's build recipe is as shipped by its package, and the installed modules are the stock ones
