## Purpose

Gives the user one command to take a GPU out of service safely and one to bring it back, coordinating application migration, kernel release and power so the device can be removed without crashing anything.

## ADDED Requirements

### Requirement: User can request a detach
The system SHALL provide a command that requests the detach of a GPU identified by its PCI address. A detach SHALL cover every function of that PCI device, including its audio function.

#### Scenario: Detach of an idle GPU
- **WHEN** the user requests a detach of a GPU that has no clients
- **THEN** the device's driver is suspended or unbound, the device is powered off through its power backend, and the command reports the final state

#### Scenario: Unknown device
- **WHEN** the user requests a detach of a PCI address that is not a managed GPU
- **THEN** the command fails with an error naming the address and nothing is changed

### Requirement: Clients are discovered before detach
Before detaching, the system SHALL identify every process that has the GPU open and classify it as migratable, non-migratable, or a display server.

#### Scenario: Status lists clients
- **WHEN** the user asks for the status of a GPU
- **THEN** the output lists each client process with its ID, name and classification

### Requirement: Blockers refuse the detach
The system SHALL refuse a detach while any non-migratable client or display server has the GPU open. The refusal SHALL use the error `ZSSDetachBlocked`, SHALL name every blocking process and why it blocks, and SHALL leave every application and the device untouched. The dry-run backend relaxes this for processes outside the layer, as its own requirement states.

#### Scenario: Application without the layer blocks
- **WHEN** a detach is requested while a process started without the ZSS layer has the GPU open
- **THEN** the request fails with `ZSSDetachBlocked`, the process is named, and no application has been migrated

#### Scenario: Display server blocks
- **WHEN** a detach is requested while a display server has the GPU open
- **THEN** the request fails with `ZSSDetachBlocked` and the display server is named as the blocker

### Requirement: Clients are moved before the device is released
The system SHALL ask every migratable client to migrate to an available GPU, or to park if none is suitable, and SHALL suspend or unbind the device's driver only after every client has reported migrated or parked.

#### Scenario: All clients migrate
- **WHEN** a detach is requested and every client can run on another GPU
- **THEN** every client reports migrated before the driver is suspended or unbound

#### Scenario: Some clients park
- **WHEN** a detach is requested and one client cannot run on any other GPU
- **THEN** that client is parked, the others are migrated, and the detach proceeds

#### Scenario: A migration fails
- **WHEN** a client reports a failed migration during a detach
- **THEN** the detach is aborted, the device stays attached and powered, the failing client is named, and clients that already migrated stay where they are

### Requirement: The driver is released the way the device requires
The system SHALL release a device's kernel driver using the strategy its power backend declares. With suspend in place, the driver SHALL stay bound and be suspended before power-off and resumed after power-on, and the device SHALL never be reported as safe to remove. With unbind, no driver SHALL be bound while the device is powered off.

#### Scenario: Suspend in place
- **WHEN** a detach completes on a device whose backend declares suspend in place
- **THEN** the driver is still bound, the device is powered off, and the reported state is powered-off

#### Scenario: Unbind
- **WHEN** a detach completes on a device whose backend declares unbind
- **THEN** no driver is bound to any function of the device

#### Scenario: Suspend fails
- **WHEN** the driver cannot be suspended
- **THEN** power is not cut, the detach fails with the driver's error, and the device stays attached

### Requirement: Safe-to-remove is reported only when it is true
The system SHALL report a device as safe to remove only when all three hold: no process has the device open, no kernel driver is bound to the device, and the power backend confirms the device is powered off and that physical removal is supported. When the backend cannot make removal electrically safe, the system SHALL report the device as powered off but not removable.

#### Scenario: Removable device
- **WHEN** a detach completes on a device whose backend supports safe removal
- **THEN** the reported state is safe-to-remove

#### Scenario: Device that cannot be removed safely
- **WHEN** a detach completes on a device whose backend cannot guarantee electrical safety
- **THEN** the reported state is powered-off and the output says physical removal is not supported

#### Scenario: Power-off not confirmed
- **WHEN** the device has been released but the backend does not confirm power-off
- **THEN** the system does not report safe-to-remove and reports the backend error

### Requirement: Attach restores the earlier arrangement
The system SHALL provide a command that requests the attach of a detached GPU. An attach SHALL power the device on, wait for the kernel driver to be ready (resumed, or bound after a rescan), migrate back every application that originated on that GPU, and resume every application that was parked for it. A device that reappears on its own while detached SHALL be treated as an attach request.

#### Scenario: Attach after detach
- **WHEN** the user requests an attach of a detached GPU
- **THEN** the device is powered and its driver is ready, applications that originated on it render from it again, and parked applications are running

#### Scenario: Applications that never used the GPU are left alone
- **WHEN** an attach completes
- **THEN** applications that originated on another GPU have not been moved

#### Scenario: Device returns by itself
- **WHEN** a detached device reappears on the bus without a command
- **THEN** the system performs the same steps as an attach request

#### Scenario: Driver does not become ready
- **WHEN** an attach is requested and the driver does not resume or bind within the timeout
- **THEN** the attach fails with an error, applications stay where they are, and parked applications stay parked

### Requirement: Device state is observable
The system SHALL expose the current state of each managed GPU as one of: attached, detaching, powered-off, safe-to-remove, attaching. It SHALL emit an event on every state change that other programs can subscribe to.

#### Scenario: State query
- **WHEN** the user asks for the status of a managed GPU
- **THEN** the output includes its current state

#### Scenario: State change event
- **WHEN** a GPU moves from one state to another
- **THEN** subscribers receive an event carrying the PCI address, the old state and the new state

### Requirement: Only authorised users can detach or attach
The system SHALL accept detach, attach and resume requests only from root, from members of a configured group, or from the user the daemon itself runs as unless that is disabled, and SHALL reject others without changing anything. Application registration SHALL be accepted from any local user.

#### Scenario: Unauthorised detach
- **WHEN** a user outside the configured group requests a detach
- **THEN** the request is rejected with a permission error and the device is unchanged
