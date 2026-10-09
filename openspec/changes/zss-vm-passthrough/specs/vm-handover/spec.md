## Purpose

Lending a GPU that ZSS manages to a virtual machine while the host keeps running, and taking it back: what the user asks for, what state the card and the host's applications are in at each step, and what happens when a step fails.

## ADDED Requirements

### Requirement: A managed GPU can be lent
The system SHALL provide an operation that takes an attached managed GPU to a state named "lent", in which no host driver is bound to any function of the card, every function is bound to the kernel's passthrough driver, and the card is powered.

#### Scenario: Lending an attached card
- **WHEN** the user lends an attached card that passes the preflight check
- **THEN** applications under ZSS_AirLock are moved off it, services that hold it only to keep it initialised are stopped, the host driver is unbound from every function of the card, every function is bound to the passthrough driver, the card is powered, and the card's state is reported as "lent"

#### Scenario: Lending a card that is off
- **WHEN** the user lends a card that is in the "off" state
- **THEN** the operation is refused, nothing is changed, and the error says to switch the card on without returning applications to it and then lend it (a driver has to be awake to let go of its device)

#### Scenario: A virtual machine can take a lent card
- **WHEN** a card is "lent" and a virtual machine configured to pass that card through is started
- **THEN** the virtual machine starts with the card assigned to it, without any further action through ZSS

### Requirement: Devices sharing the card's isolation group go with it only when asked
The system SHALL hand devices that share the card's isolation group (bridges excepted) to the passthrough driver together with the card only when the user asks for it, and SHALL give them back to their host drivers when the card is reclaimed or the hand-over fails.

#### Scenario: Asked for
- **WHEN** the user lends a card and asks for its group to go with it
- **THEN** every such device loses its host driver for as long as the card is lent, and has it again after reclaim

#### Scenario: Not asked for
- **WHEN** the user lends a card that shares its group, without asking for the group to go with it
- **THEN** lending is refused and nothing is changed

### Requirement: Lending is refused when it cannot be done safely
The system SHALL refuse to lend a card when the preflight check finds an obstacle, SHALL change nothing in that case, and SHALL name every obstacle found.

#### Scenario: Obstacle present
- **WHEN** the user lends a card and the preflight check reports one or more obstacles
- **THEN** the operation fails, the card and all applications are as they were, and the error lists each obstacle

#### Scenario: A program that cannot be moved holds the card
- **WHEN** a program that was not started under ZSS_AirLock, or one ZSS_AirLock cannot move, has the card open
- **THEN** lending is refused and the program is named; it is not frozen as it would be for a power-off, because a frozen program still has the card open and the host driver could not let go

### Requirement: A failed hand-over is undone
The system SHALL return the card to the host driver and applications to the card when any step of lending fails after applications have been moved.

#### Scenario: Binding the passthrough driver fails
- **WHEN** applications have been moved and the host driver unbound, and a function of the card cannot be bound to the passthrough driver
- **THEN** the host driver is bound again to every function, applications are returned as after a power-on, the state is "attached", and the error names the function and the step

### Requirement: A lent card is left alone
While a card is "lent" the system SHALL NOT wake it, power it off, read its configuration for loss detection, or report it lost.

#### Scenario: The guest resets or powers down the card
- **WHEN** a virtual machine holding a lent card resets it or stops answering through it
- **THEN** the host reports no loss, moves no application, and takes no action on the card

#### Scenario: A host program asks for the card
- **WHEN** a host program started for the dedicated GPU starts while the card is lent
- **THEN** it starts on the fallback GPU, as it does while the card is off, and is recorded as wanting the card

### Requirement: A lent card can be reclaimed
The system SHALL provide an operation that takes a "lent" card back: the card is reset by a power cycle where the power backend allows one, bound to the host driver again, and applications that were moved away or want the card are returned as after a power-on.

#### Scenario: Reclaiming after the virtual machine has stopped
- **WHEN** no virtual machine holds the card and the user reclaims it
- **THEN** the card is power-cycled, the host driver is bound to every function, the state is "attached", and applications are returned unless the user asked for them to stay

#### Scenario: Reclaiming while a virtual machine holds the card
- **WHEN** a virtual machine still holds the card and the user reclaims it
- **THEN** the operation is refused, the virtual machine is not disturbed, and the error names the process holding the card

#### Scenario: The card cannot be reset, or the host driver does not come back
- **WHEN** the power cycle fails, or the host driver fails to bind after it
- **THEN** the state stays "lent", the failure is reported, applications stay where they are, and reclaiming can be attempted again

### Requirement: The state survives a daemon restart
The system SHALL recognise a lent card after the daemon restarts and SHALL NOT bind a host driver to it or power it off on start-up or shutdown.

#### Scenario: Daemon restarted while a guest runs
- **WHEN** the daemon is restarted while a virtual machine holds a lent card
- **THEN** the card is reported as "lent", the virtual machine keeps running, and reclaiming works afterwards

### Requirement: Status shows the lent state and its holder
The status output SHALL report a lent card as "lent" and SHALL name the process holding it, if any.

#### Scenario: Status with a running guest
- **WHEN** the user asks for status while a virtual machine holds a lent card
- **THEN** the card is shown as "lent" and the virtual machine's process is listed as its holder

### Requirement: ZSS does not manage virtual machines or the bootloader
The system SHALL NOT start, stop or configure a virtual machine, and SHALL NOT modify the bootloader's configuration or the kernel command line.

#### Scenario: IOMMU is disabled
- **WHEN** the IOMMU is not enabled
- **THEN** the system reports that and the kernel parameter that enables it, and changes no file
