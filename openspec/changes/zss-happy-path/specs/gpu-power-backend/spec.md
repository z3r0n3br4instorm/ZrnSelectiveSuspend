## Purpose

Separates how a GPU is physically powered from the rest of ZrnSelectiveSuspend, so the same detach and attach sequences work on any platform that can supply a backend, and so each platform states honestly whether pulling the device is safe.

## ADDED Requirements

### Requirement: Backends share one contract
Every power backend SHALL be able to power a device off, power it on, report whether the device is currently powered, and declare whether physical removal is electrically safe once it is powered off.

#### Scenario: Power-off is confirmed
- **WHEN** a backend is asked to power a device off and succeeds
- **THEN** a subsequent query reports the device as not powered

#### Scenario: Power-off fails
- **WHEN** a backend cannot power a device off
- **THEN** it returns an error describing the cause and reports the device as still powered

### Requirement: A backend is selected per device
The system SHALL choose a backend for each managed GPU from configuration when one is given, and otherwise by detecting the platform. When no backend applies, the system SHALL refuse to manage the device's power and say so.

#### Scenario: Configured backend
- **WHEN** configuration names a backend for a device
- **THEN** that backend is used for the device

#### Scenario: No backend available
- **WHEN** a detach is requested for a device with no applicable backend
- **THEN** the request fails with an error stating that no power backend is available

### Requirement: Power is never cut on a device in use
A backend SHALL be asked to power a device off only after the kernel driver has let go of that device, either by being unbound or by being suspended.

#### Scenario: Driver active
- **WHEN** a power-off is attempted while a kernel driver is bound to the device and not suspended
- **THEN** the power-off is refused and the device stays powered

#### Scenario: Driver suspended
- **WHEN** a power-off is attempted while the bound driver is suspended
- **THEN** the power-off proceeds

### Requirement: Each backend declares its release strategy
Every backend SHALL declare whether its devices are released by suspending the driver in place or by unbinding it. A backend whose devices can be physically removed SHALL declare unbind.

#### Scenario: Removable device
- **WHEN** a backend declares that physical removal is safe
- **THEN** its release strategy is unbind

### Requirement: Hot-plug slot backend
The system SHALL provide a backend for devices in a PCIe slot that has its own power control. It SHALL declare physical removal safe and SHALL release devices by unbinding.

#### Scenario: Slot powered off
- **WHEN** the hot-plug slot backend powers off a device
- **THEN** the slot reports no power and the device is absent from the bus

#### Scenario: Slot powered on
- **WHEN** the hot-plug slot backend powers on a slot that holds a device
- **THEN** the device appears on the bus

### Requirement: Apple gmux backend
The system SHALL provide a backend for the discrete GPU of Apple laptops with a classic gmux. It SHALL declare physical removal not supported, because the GPU is soldered, and SHALL release the device by suspending its driver in place. It SHALL be selectable only on machines where a classic gmux is detected.

#### Scenario: Discrete GPU powered off
- **WHEN** the gmux backend powers off the discrete GPU
- **THEN** the gmux reports the discrete GPU as unpowered

#### Scenario: Non-Apple machine
- **WHEN** the gmux backend is configured on a machine without a classic gmux
- **THEN** the system refuses to use it and reports why

### Requirement: Dry-run backend
The system SHALL provide a backend that changes no hardware state. With it, a detach SHALL migrate and park applications as usual but SHALL leave the device's driver active and the device powered, and SHALL report that power was not changed. Because a dry run takes nothing away from them, processes that were not started under the ZSS layer SHALL be listed but SHALL NOT block a dry-run detach.

#### Scenario: Dry-run detach
- **WHEN** a detach is requested for a device using the dry-run backend
- **THEN** its applications are migrated or parked, the driver stays active, the device stays powered, and the reported state says the run was a dry run

#### Scenario: Display server present during a dry run
- **WHEN** a dry-run detach is requested while a display server has the GPU open
- **THEN** the display server is listed as ignored, and applications under the layer are still migrated

#### Scenario: Non-migratable application during a dry run
- **WHEN** a dry-run detach is requested while an application under the layer uses an untracked feature
- **THEN** the request fails with `ZSSDetachBlocked` naming that application
