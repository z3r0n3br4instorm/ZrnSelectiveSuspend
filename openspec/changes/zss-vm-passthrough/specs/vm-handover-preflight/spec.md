## Purpose

Telling the user, without changing anything, whether a particular GPU on this machine can be lent to a virtual machine, and for each thing in the way, what it is and what would remove it.

## ADDED Requirements

### Requirement: A read-only check
The system SHALL provide a check for a given card that changes no driver binding, power state, file or application, and that can be run at any time by any user allowed to query status.

#### Scenario: Running the check
- **WHEN** the user runs the check for a card
- **THEN** the card, its driver bindings, its power state and all applications are exactly as before

### Requirement: Every obstacle is reported, with its remedy
The check SHALL report all obstacles it finds, not only the first, each with a one-line reason and a one-line remedy, and SHALL exit with a failure status if there is any.

#### Scenario: IOMMU disabled
- **WHEN** the kernel has no IOMMU enabled
- **THEN** the check reports it, says whether the firmware offers one, and names the kernel parameter that enables it

#### Scenario: The card shares its isolation group
- **WHEN** a device that is not a function of the card is in the same isolation group
- **THEN** the check reports each such device by address and name

#### Scenario: The display server holds the card
- **WHEN** the display server has the card open
- **THEN** the check reports it and says the card cannot be lent while that is so

#### Scenario: A function has no passthrough driver available
- **WHEN** the passthrough driver is not available for a function of the card
- **THEN** the check reports the function and what is missing

#### Scenario: A program blocks the hand-over
- **WHEN** a program holds the card that would block a power-off
- **THEN** the check lists it with the same reason a power-off would give

### Requirement: A clean result says what will happen
When there is no obstacle the check SHALL list every function of the card that will be handed over and every application that will be moved or frozen, and SHALL exit successfully.

#### Scenario: Nothing in the way
- **WHEN** the check finds no obstacle
- **THEN** it lists the card's functions with their current drivers, the applications that will be moved and those that will be frozen, and exits with success

### Requirement: The result is available to programs
The check's result SHALL be available in a machine-readable form with the same content as the text.

#### Scenario: Machine-readable output
- **WHEN** the check is run with the machine-readable option
- **THEN** it prints one structured document listing the functions, the obstacles with reason and remedy, and the affected applications
