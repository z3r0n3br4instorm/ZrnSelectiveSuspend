## Purpose

Notices inside the kernel that a managed device has disappeared, keeps its driver from talking to hardware that is gone, and tells user space at once.

## ADDED Requirements

### Requirement: A device that goes silent is detected quickly
While a managed device is supposed to be on, the module SHALL detect that it has stopped answering within 300 milliseconds.

#### Scenario: Device stops answering
- **WHEN** a managed device that is on stops answering on the bus
- **THEN** its state reads "lost" within 300 milliseconds

#### Scenario: Device that is off
- **WHEN** a managed device is off
- **THEN** its silence is not reported as a loss

### Requirement: A lost device is marked disconnected
On detecting a loss the module SHALL mark every function of the device disconnected in the kernel, so that configuration reads are answered without touching hardware and drivers that check for an offline device stop using it.

#### Scenario: Marked offline
- **WHEN** a loss is detected
- **THEN** the kernel reports each function of the device as offline

### Requirement: The driver is told
On detecting a loss the module SHALL notify a driver that implements PCI error recovery that its device has failed permanently. When the device is taken back, the module SHALL ask such a driver to reinitialise it, and SHALL report a driver that has no such handlers so that user space can rebind it.

#### Scenario: Driver with error handlers
- **WHEN** a loss is detected on a device whose driver implements PCI error recovery
- **THEN** the driver's error handler is called with the permanent-failure state

#### Scenario: Driver without error handlers
- **WHEN** a lost device is taken back and its driver has no recovery handlers
- **THEN** the module reports that the driver needs rebinding, and the daemon rebinds it

### Requirement: A loss is announced
The module SHALL announce a loss to user space with the same kind of event as any other state change.

#### Scenario: Event on loss
- **WHEN** a loss is detected
- **THEN** an event carrying the device's address and the state "lost" is sent

### Requirement: A returning device can be taken back
When a lost device answers again, the module SHALL allow it to be powered on through the normal sequence, clearing the disconnected mark.

#### Scenario: Device returns
- **WHEN** a lost device answers again and "on" is written
- **THEN** the mark is cleared, its state is restored, and the state reads "on"
