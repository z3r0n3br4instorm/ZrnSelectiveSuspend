## Purpose

Lets an application started under the ZSS layer keep running when the GPU it renders on is lost without warning, by rebuilding its graphics state elsewhere from what the layer already holds.

## ADDED Requirements

### Requirement: A lost device is not reported to the application
When a real driver reports that its device is lost, or the daemon asks the application to evacuate, the layer SHALL NOT return a device-lost error to the application while recovery is possible. It SHALL hold the application's Vulkan calls, recover, and then let them complete.

#### Scenario: Device lost during a submit
- **WHEN** the real driver reports device loss from a call the application made
- **THEN** that call returns success after recovery, and the process ID is unchanged

#### Scenario: No daemon
- **WHEN** the real driver reports device loss and no daemon is reachable
- **THEN** the layer recovers onto a GPU of its own choosing, or returns the device-lost error if none is suitable

### Requirement: State is rebuilt without reading from the lost device
Recovery SHALL recreate every tracked object on the target from information held outside the lost device, and SHALL NOT depend on any read from the lost device succeeding.

#### Scenario: Objects and commands survive
- **WHEN** an application with pipelines, descriptor sets and a recorded command buffer is recovered
- **THEN** it renders with them on the target without recreating any of them itself

#### Scenario: Data written through mapped memory survives
- **WHEN** an application that filled a buffer through mapped memory is recovered
- **THEN** the buffer holds the same bytes on the target

#### Scenario: Uploaded textures survive
- **WHEN** an application that uploaded a texture from a buffer is recovered
- **THEN** the texture holds the same contents on the target

### Requirement: Work in flight is re-issued
A submit that was lost with the device SHALL be issued again on the target. Fences and semaphores the application was waiting on for lost work SHALL become signalled, so the application cannot wait forever.

#### Scenario: Frame in flight at the moment of loss
- **WHEN** the device is lost while a frame's commands are being submitted
- **THEN** that frame is rendered on the target

#### Scenario: Waiting on lost work
- **WHEN** the application waits on a fence belonging to work that was lost
- **THEN** the wait returns success after recovery

### Requirement: Unrecoverable contents are zeroed and reported
Images and buffers whose contents were produced by the GPU and are held nowhere else SHALL be recreated filled with zeros. The layer SHALL report to the daemon, and log, how many such objects there were.

#### Scenario: Accumulated render target
- **WHEN** an application that accumulates into an image across frames is recovered
- **THEN** the image is zero-filled on the target, and the recovery report counts one object with lost contents

#### Scenario: Nothing lost
- **WHEN** an application whose images are all uploaded or redrawn each frame is recovered
- **THEN** the recovery report counts no objects with lost contents

### Requirement: The target follows the same rules as a migration
Recovery SHALL use the target named by the daemon when it is suitable, SHALL park the application when no suitable target exists, and SHALL resume a parked application when a suitable GPU returns. Rebuilding on the same GPU SHALL be possible when that GPU is still present.

#### Scenario: Evacuate to another GPU
- **WHEN** the daemon names another GPU that satisfies the application's capabilities
- **THEN** the application continues on that GPU

#### Scenario: Nowhere to go
- **WHEN** the device is lost and no suitable GPU exists
- **THEN** the application is parked, and it resumes when its origin GPU returns

#### Scenario: Reset in place
- **WHEN** the driver reports device loss while the GPU is still present
- **THEN** the application is rebuilt on the same GPU

### Requirement: The origin is remembered across a loss
An application recovered onto another GPU SHALL be moved back to its origin GPU when that GPU is attached again, exactly as after a migration.

#### Scenario: Card returns after a loss
- **WHEN** the lost GPU is attached again
- **THEN** the application renders from it again

### Requirement: A stuck thread does not block recovery
If an application thread does not return from the lost driver within a grace period, recovery SHALL proceed without it. The lost device SHALL then be abandoned rather than destroyed, and that thread SHALL join the recovered state if it later returns.

#### Scenario: Thread blocked in the dead driver
- **WHEN** one thread stays inside a call to the lost driver past the grace period
- **THEN** the application's other threads continue on the target

### Requirement: Non-migratable applications are not recovered
An application marked non-migratable SHALL receive the device-lost error as it would without the layer.

#### Scenario: Untracked feature in use
- **WHEN** the device is lost under an application that uses an untracked feature
- **THEN** the application receives the device-lost error
