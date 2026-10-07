## Purpose

Takes a PCI GPU and its sibling functions through quiesce, state save, power-off and back, inside the kernel, for any driver and any platform a backend exists for.

## ADDED Requirements

### Requirement: The module is inert until a device is handed to it
Loading the module SHALL NOT change the state of any device. A device SHALL be managed only after it is named explicitly, and SHALL stop being managed on request.

#### Scenario: Load
- **WHEN** the module is loaded
- **THEN** no device is managed and no device's power or driver state has changed

#### Scenario: Manage and unmanage
- **WHEN** a PCI address is written to the manage file and later to the unmanage file
- **THEN** a directory for that device appears and then disappears, and the device is powered and usable afterwards

#### Scenario: Unknown device
- **WHEN** an address that is not a PCI device is written to the manage file
- **THEN** the write fails and nothing is created

### Requirement: A managed device can be powered off and on
The module SHALL power a managed device off by quiescing its driver, saving its PCI state, and cutting power through its backend, and SHALL power it on by restoring power, waiting for the device to answer, restoring its state, and resuming its driver. All functions sharing the device's slot SHALL be handled together.

#### Scenario: Off
- **WHEN** "off" is written to a managed device's power file while the device is idle
- **THEN** its driver is quiesced, power is cut, and the state reads "off"

#### Scenario: On
- **WHEN** "on" is written while the state is "off"
- **THEN** power is restored, the PCI configuration is as it was before, the driver works again, and the state reads "on"

#### Scenario: Sibling function
- **WHEN** a GPU with an audio function is powered off and on
- **THEN** both functions are quiesced before power is cut and both work afterwards

### Requirement: A failed power-off leaves the device on
If any step before the power cut fails, the module SHALL undo the steps already taken and leave the device powered and usable, and SHALL report which step failed.

#### Scenario: Driver refuses to quiesce
- **WHEN** the driver's sleep callback returns an error
- **THEN** the write fails, the state reads "on", the device works, and the reason is readable

#### Scenario: Backend does not cut power
- **WHEN** the backend reports success but the device still answers
- **THEN** the driver is resumed, the state reads "on", and the reason is readable

### Requirement: A device that does not come back is reported
If the device does not answer within a bounded time after power is restored, the module SHALL report the failure and SHALL allow another attempt.

#### Scenario: No answer after power-on
- **WHEN** power is restored and the device does not answer in time
- **THEN** the state reads "failed" with the reason readable, and a later "on" may succeed

### Requirement: Quiesce is vendor-neutral
The module SHALL quiesce a device through its driver's own system-sleep callbacks, and SHALL offer a mode in which user space quiesces the driver itself and the module handles state and power only.

#### Scenario: Open driver
- **WHEN** a device bound to a driver with system-sleep callbacks is powered off with the "pm" strategy
- **THEN** those callbacks run in the order a system sleep runs them, and their counterparts run in reverse on power-on

#### Scenario: Externally quiesced driver
- **WHEN** a device is managed with the "external" strategy
- **THEN** the module calls no driver callback and performs only the state and power steps

### Requirement: A device nothing would reinitialise is not powered off
With the "pm" strategy the module SHALL refuse to power off a device that has no driver bound, since a device comes back from a power cut uninitialised and only its driver replays its firmware's initialisation.

#### Scenario: No driver
- **WHEN** "off" is written for a device with no driver bound, managed with the "pm" strategy
- **THEN** the write fails, the reason is readable, and the device stays on

### Requirement: The device cannot start transfers while losing power
Before power is cut the module SHALL stop the device from initiating transfers on the bus, and SHALL restore that ability when the device is back. The module SHALL report whether the device's transfers are confined by an IOMMU.

#### Scenario: Bus mastering
- **WHEN** a device that was allowed to initiate transfers is powered off and on
- **THEN** it cannot initiate transfers while off and can again afterwards

### Requirement: Power backends are pluggable
The module SHALL select a power backend per device from those that recognise the platform, and SHALL accept an explicit choice.

#### Scenario: No backend
- **WHEN** a device is handed to the module on a platform no backend recognises and none is named
- **THEN** the write fails, saying that no power backend applies

### Requirement: State changes are announced
Every change of a managed device's state SHALL be announced to user space as a kernel event naming the device, the new state and the reason.

#### Scenario: Event on power-off
- **WHEN** a managed device is powered off
- **THEN** an event carrying its address and the state "off" is sent

### Requirement: Devices are restored when the module goes
When the module is unloaded, every managed device that is off SHALL first be powered on and restored.

#### Scenario: Unload with a device off
- **WHEN** the module is unloaded while a managed device is off
- **THEN** the device is powered, restored and usable before the unload completes

### Requirement: The daemon uses the module when present
When the module is loaded, the daemon SHALL be able to power a device off and on through it, and SHALL behave as before when the module is absent.

#### Scenario: Module present
- **WHEN** the daemon manages a device with the module loaded and no backend named
- **THEN** off and on go through the module, and the daemon's rules, progress report and recovery behave as before

#### Scenario: Module absent
- **WHEN** the module is not loaded
- **THEN** the daemon uses its own backends
