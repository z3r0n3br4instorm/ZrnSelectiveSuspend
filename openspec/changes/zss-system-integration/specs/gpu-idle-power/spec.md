## Purpose

Lets a discrete GPU that cannot be removed be powered off while the desktop keeps running, and brings it back by itself the moment anything needs it.

## ADDED Requirements

### Requirement: The user can power a GPU off and on
The system SHALL provide commands that power a managed suspend-in-place GPU off and on. Powering off SHALL suspend the device's driver before cutting power, and powering on SHALL restore power before resuming the driver. Neither SHALL switch the screen away from the user's session.

#### Scenario: Power off under a running desktop
- **WHEN** the user powers off an idle GPU while a display server has it open
- **THEN** the device is powered off, the screen stays on the user's session, and the reported state is powered-off

#### Scenario: Power on
- **WHEN** the user powers on a GPU that is powered off
- **THEN** the device is powered, its driver is resumed, and the reported state is attached

#### Scenario: Driver will not suspend
- **WHEN** the driver refuses to suspend
- **THEN** power is not cut, the request fails with the driver's error, and the device stays attached

### Requirement: Only a wake-capable driver is powered off under a display server
The system SHALL power a GPU off while a display server holds it only if the device's driver can signal that a caller is waiting for it. Otherwise the display server SHALL block the request, as it does for a detach.

#### Scenario: Stock driver
- **WHEN** a power-off is requested for a GPU whose driver cannot signal waiting callers and a display server holds the device
- **THEN** the request is refused, naming the display server and stating that the driver lacks wake support

### Requirement: Idle holders do not block
A display server that is not driving a display from the GPU, and services that hold the device only to keep it initialised, SHALL NOT block a power-off of a suspend-in-place GPU. Such services SHALL be stopped before the power-off and started again after power-on.

#### Scenario: Persistence service
- **WHEN** a GPU is powered off while a service that only keeps the device initialised is running
- **THEN** that service is stopped before power is cut and is running again after power-on

### Requirement: Applications are moved first
Before a power-off, every application rendering on the GPU that the ZSS layer can migrate SHALL be migrated or parked as for a detach.

#### Scenario: Application under the layer
- **WHEN** a GPU is powered off while an application under the ZSS layer renders on it
- **THEN** the application is migrated before power is cut and keeps rendering

### Requirement: Processes that cannot be moved are frozen while the GPU is off
When the user requests a power-off, a process that uses the GPU and cannot be migrated (one not started under the ZSS layer, or an application the layer cannot move) SHALL be frozen before the driver is suspended and SHALL be thawed after the driver is resumed, whatever caused the power-on. The report SHALL name every process frozen. If a process cannot be frozen, power SHALL NOT be cut and the refusal SHALL name it. A process that the requesting command was run from SHALL NOT be frozen; it SHALL block the request instead, unless the text console was requested. An automatic power-off SHALL never freeze a process: such a process counts as use of the GPU.

#### Scenario: Process outside the layer
- **WHEN** the user powers a GPU off while a process not started under the ZSS layer has it open
- **THEN** that process is frozen, the GPU is powered off, and the report names the process

#### Scenario: Power-on
- **WHEN** the GPU is powered on again, by command or by a wake request
- **THEN** every process frozen for the power-off is running again

#### Scenario: Process cannot be frozen
- **WHEN** a process that must be frozen cannot be
- **THEN** any process already frozen is thawed, the request fails naming the process, and the device stays powered

#### Scenario: The terminal the command was typed in
- **WHEN** the process using the GPU is the terminal that `zssctl off` was run from
- **THEN** the request is refused, saying why, and nothing is frozen

#### Scenario: Idle timer
- **WHEN** the idle timeout expires while a process outside the layer has the GPU open
- **THEN** the GPU stays powered and the process is not frozen

#### Scenario: Daemon killed
- **WHEN** the daemon is killed while processes are frozen for a power-off
- **THEN** the recovery step thaws them after restoring the device

### Requirement: A GPU driving a display is not powered off
The system SHALL refuse to power off a GPU that is driving a connected display.

#### Scenario: External monitor in use
- **WHEN** a power-off is requested while an external display driven by the GPU is active
- **THEN** the request is refused, stating that the device is driving a display

### Requirement: A wake request powers the GPU on
While a GPU is powered off, the system SHALL notice when anything calls into its suspended driver. If the GPU was powered off by the idle timer, the system SHALL power it on and resume the driver for any caller. If it was powered off on request and the caller is the display server, the system SHALL power it for the duration of the call and power it off again without a command, leaving it hidden and its state reported as powered-off. If the driver cannot say who is calling, the system SHALL power the GPU on. The caller SHALL then complete normally.

#### Scenario: Display query while off
- **WHEN** a program asks the display server about displays while the GPU is powered off
- **THEN** the GPU is powered on, the query completes, and the reported state is attached

### Requirement: A GPU switched off on request stays off
A GPU powered off on request SHALL stay off when a program other than the display server calls into its driver. That program SHALL wait until the GPU is powered on, SHALL be named in the system log, and SHALL be counted in the status output.

#### Scenario: Monitoring tool
- **WHEN** a program other than the display server calls into the driver of a GPU that was powered off on request
- **THEN** the GPU stays off, the program waits, and the log names it and says how to power the GPU on

#### Scenario: Display server calls in
- **WHEN** the display server calls into the driver of a GPU that was powered off on request
- **THEN** the call completes, the GPU is powered off again within seconds, and the status counts the occasion

#### Scenario: Calls in quick succession
- **WHEN** the display server calls again shortly after the GPU went off again
- **THEN** the GPU stays powered longer before going off, rather than cycling for each call

#### Scenario: Display now in use
- **WHEN** the GPU has started driving a display by the time it should go off again
- **THEN** it stays on, is no longer hidden, and the log says why

#### Scenario: Power-on releases the waiters
- **WHEN** the GPU is powered on
- **THEN** every waiting program continues

#### Scenario: Idle timer
- **WHEN** any program calls into the driver of a GPU that the idle timer powered off
- **THEN** the GPU is powered on

#### Scenario: Time to wake
- **WHEN** a wake request arrives
- **THEN** the driver is resumed within three seconds

#### Scenario: Returned applications
- **WHEN** a GPU is woken by a wake request
- **THEN** applications that were migrated away from it stay where they are until an attach is requested

### Requirement: A GPU switched off on request is hidden from new programs
While a GPU is powered off on request, the system SHALL hide it from programs that start afterwards: its device nodes, the files that announce its vendor driver to the graphics loaders, and the vendor driver's own status files SHALL read as empty. Such programs SHALL then run on another GPU or report that the device is unavailable, without waking the GPU and without waiting on it. Everything hidden SHALL be visible again when the GPU is powered on, when the daemon stops, and after the recovery step. A GPU powered off by the idle timer SHALL NOT be hidden.

#### Scenario: Program started while the GPU is off
- **WHEN** a Vulkan or OpenGL program is started while the GPU is powered off on request
- **THEN** it runs on another GPU and the GPU stays powered off

#### Scenario: Vendor tool
- **WHEN** the vendor's status tool is run while the GPU is powered off on request
- **THEN** it reports that it cannot reach the device, and the GPU stays powered off

#### Scenario: Power-on
- **WHEN** the GPU is powered on
- **THEN** nothing is hidden any more and new programs can use it

#### Scenario: Daemon killed
- **WHEN** the daemon is killed while a GPU is hidden
- **THEN** the recovery step makes it visible again

### Requirement: A GPU can be powered off when idle
When an idle timeout is configured, the system SHALL power a GPU off after it has had no client and has driven no display for that long. With no timeout configured the system SHALL never power a GPU off on its own.

#### Scenario: Idle timeout reached
- **WHEN** a GPU has been unused for the configured idle timeout
- **THEN** it is powered off without a command

#### Scenario: Used again before the timeout
- **WHEN** an application starts rendering on the GPU before the timeout expires
- **THEN** the GPU stays powered

#### Scenario: No timeout configured
- **WHEN** no idle timeout is configured
- **THEN** the GPU is powered off only on request

### Requirement: Repeated waking is damped
If a GPU is woken again shortly after each automatic power-off, the system SHALL lengthen the time it waits before the next automatic power-off, and SHALL report how often the GPU has been woken.

#### Scenario: Something polls the display server
- **WHEN** a GPU is woken within a short time of three automatic power-offs in a row
- **THEN** the next automatic power-off is delayed beyond the configured timeout

### Requirement: Every step of a power transition is reported
While a GPU is powered off or on, the system SHALL report each step as it happens, prefixed `[ZrnSelectiveSuspend]`: the device and its driver, who is using it, what was moved or stopped, the driver being suspended or resumed, power being cut or restored, how long it took, and how to bring the device back. The report SHALL go to the command that asked and to the system log. A failed step SHALL be reported with its reason.

#### Scenario: Power-off from the command line
- **WHEN** the user powers a GPU off
- **THEN** the command prints a line naming the device, one numbered line per step, and a final line saying the device is powered off and how to power it on

#### Scenario: A step fails
- **WHEN** the driver refuses to suspend during a power-off
- **THEN** the report says which step failed, why, and that power was not cut

#### Scenario: Woken by a request
- **WHEN** a GPU is powered on because something called into its driver
- **THEN** the system log says so, with how long the device had been off

### Requirement: Progress can be shown on a text console
On request, the system SHALL switch the screen to a text console for a power-off, print the same report there, and keep the screen there while the device is off. It SHALL return the screen to where it was when the device is powered on, and SHALL power the device on when a key is pressed on that console. In this mode a display server SHALL NOT block the power-off even if the driver has no wake support, since the desktop is not on screen.

#### Scenario: Console mode
- **WHEN** the user powers a GPU off in console mode
- **THEN** the screen shows a text console with each step, and a line saying that pressing a key powers the device on

#### Scenario: Return from the console
- **WHEN** the GPU is powered on again
- **THEN** the screen returns to the session it was on before

### Requirement: Power is restored when the daemon stops
When the daemon stops or fails, a GPU it had powered off SHALL be powered on and its driver resumed, so that no caller is left waiting.

#### Scenario: Service stopped while the GPU is off
- **WHEN** the daemon is stopped while a GPU is powered off
- **THEN** the GPU is powered on and its driver resumed before the daemon exits

#### Scenario: Daemon killed
- **WHEN** the daemon is killed while a GPU is powered off
- **THEN** the GPU is powered on and its driver resumed by the service's recovery step
