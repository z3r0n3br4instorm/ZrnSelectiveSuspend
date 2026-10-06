## Purpose

Makes the daemon tell the truth when a managed GPU disappears without having been detached, and gets the applications that were using it moved somewhere else.

## ADDED Requirements

### Requirement: An unrequested disappearance is detected
The system SHALL detect that a managed GPU in the attached state is no longer on the bus, without any command, within two seconds of it leaving.

#### Scenario: Card pulled while attached
- **WHEN** a managed GPU is removed from the bus without a detach
- **THEN** within two seconds its reported state is `lost`

#### Scenario: Orderly detach is not a loss
- **WHEN** a GPU leaves the bus as part of a detach the system is carrying out
- **THEN** its state follows the detach sequence and never becomes `lost`

### Requirement: Lost is a distinct, observable state
The system SHALL report a GPU that disappeared without a detach as `lost`, in addition to the states defined for orderly detach and attach. It SHALL emit a state-change event when a GPU becomes lost. A lost GPU SHALL NOT be reported as safe to remove or as powered off.

#### Scenario: Status of a lost device
- **WHEN** the user asks for the status of a GPU that was pulled without a detach
- **THEN** the state shown is `lost`

#### Scenario: Event on loss
- **WHEN** a GPU becomes lost
- **THEN** subscribers receive an event with the old state `attached` and the new state `lost`

### Requirement: Detach of a lost device is refused
The system SHALL refuse a detach request for a lost GPU with an error saying the device is already gone, and SHALL change nothing.

#### Scenario: Detach after the card was pulled
- **WHEN** a detach is requested for a lost GPU
- **THEN** the request fails, the error says the device is already gone, and the state remains `lost`

### Requirement: Applications are told to evacuate
When a GPU becomes lost, the system SHALL ask every registered application that was using it to evacuate, naming a target chosen as for a detach. The system SHALL also accept a loss report from an application, and SHALL then ask every application using that GPU to evacuate. When the reported GPU is still on the bus, the target SHALL be that same GPU.

#### Scenario: Card pulled under running applications
- **WHEN** a GPU with two registered applications becomes lost
- **THEN** both receive an evacuate request

#### Scenario: Application reports a loss the daemon has not seen
- **WHEN** an application reports that its device was lost while the GPU is still on the bus
- **THEN** the applications using that GPU are asked to rebuild on the same GPU, and its state stays `attached`

#### Scenario: Healthy application during a reset
- **WHEN** applications are asked to rebuild on the same GPU and one of them has not seen a loss
- **THEN** that application is left as it is

#### Scenario: Application reports a loss of a GPU that is gone
- **WHEN** an application reports that its device was lost and the GPU is no longer on the bus
- **THEN** the GPU becomes `lost` and its applications are asked to evacuate to another target

### Requirement: Stale handles remain discoverable
The system SHALL be able to list processes that still hold a GPU's device nodes after the GPU has left the bus.

#### Scenario: Process outside the layer after a loss
- **WHEN** a process not started under the ZSS layer had a GPU open and that GPU becomes lost
- **THEN** the status of the GPU lists that process as holding a stale handle

### Requirement: A lost device that returns is attached
When a lost GPU reappears on the bus, the system SHALL carry out the attach sequence without a command, unless automatic attach is disabled, in which case an attach request SHALL do so.

#### Scenario: Card returns after a loss
- **WHEN** a lost GPU reappears on the bus
- **THEN** it becomes `attached`, and applications that originated on it are moved back

#### Scenario: Attach requested while still absent
- **WHEN** an attach is requested for a lost GPU that is not on the bus
- **THEN** the request fails and the state remains `lost`
