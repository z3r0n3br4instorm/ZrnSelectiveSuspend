## Purpose

Defines what Vulkan the layer presents to an application, how that is chosen, and what becomes of an application that uses something the layer cannot move to another GPU.

## ADDED Requirements

### Requirement: The layer presents Vulkan 1.2
The layer SHALL present physical devices that report Vulkan 1.2 where the underlying driver supports it, and SHALL implement every core command of versions 1.0, 1.1 and 1.2.

#### Scenario: Version reported
- **WHEN** an application under the layer asks a physical device for its properties on a GPU whose driver supports Vulkan 1.2
- **THEN** the reported API version is at least 1.2

#### Scenario: A 1.2 application starts
- **WHEN** an application that requires Vulkan 1.2 is started under the layer
- **THEN** it creates its device and renders

#### Scenario: A 1.0 application is unaffected
- **WHEN** an application written for Vulkan 1.0 is started under the layer
- **THEN** it behaves as it did before this change, and its frames are identical

### Requirement: Nothing is offered by accident
Every command of every version and extension the layer presents SHALL be handled deliberately: tracked so that the application remains migratable, or marked as pinning the application, or handled by hand. A command with no such decision SHALL prevent the layer from being built.

#### Scenario: Unclassified command
- **WHEN** the layer is built against a registry in which an offered version or extension has a command with no classification
- **THEN** the build fails and names the command

#### Scenario: Every offered extension can be enabled
- **WHEN** a device is created with every extension the layer advertises enabled
- **THEN** creation succeeds on every GPU that the advertisement came from

### Requirement: An application is offered only what it could take with it
By default the layer SHALL report, for each physical device, only the version, features, limits, formats and extensions supported by every GPU its applications may be moved to. On request it SHALL instead report what each GPU's own driver supports.

#### Scenario: Portable profile
- **WHEN** two GPUs of different capability are in the same group and an application queries either under the default profile
- **THEN** it is offered only what both support

#### Scenario: Native profile
- **WHEN** the native profile is requested
- **THEN** each physical device reports its own driver's capabilities

#### Scenario: Profile too small to start
- **WHEN** an application cannot create its device under the portable profile
- **THEN** the layer's log names the GPU and the missing capability that limited the profile

### Requirement: Applications using the new features stay migratable
An application that uses descriptor update templates, the second render pass interface, imageless framebuffers, timeline semaphores, sampler conversions, query pools, events or buffer views SHALL remain migratable, and SHALL be rebuilt correctly on another GPU and after a loss.

#### Scenario: Migration with a new kind of object in use
- **WHEN** an application using one of these is moved to another GPU of the same driver
- **THEN** its frames afterwards are identical to an undisturbed run

#### Scenario: Timeline semaphore across a move
- **WHEN** an application waiting on a timeline semaphore value is moved
- **THEN** values already signalled remain signalled and the wait completes when the value is reached

#### Scenario: Query across a loss
- **WHEN** a GPU is lost while a query is active
- **THEN** the application is rebuilt and the query's result is reported as unavailable, not as a wrong number

### Requirement: Features that cannot move pin the application, visibly
When an application uses a feature whose state the layer cannot reproduce on a different GPU, the layer SHALL treat that application as restricted to GPUs where it can, SHALL report the reason, and SHALL NOT move it elsewhere.

#### Scenario: Buffer device address
- **WHEN** an application asks for the device address of a buffer
- **THEN** the status output shows the application as pinned and gives buffer addresses as the reason

#### Scenario: Pinned application and a power-off
- **WHEN** a power-off is requested for the GPU a pinned application is on
- **THEN** the application is frozen for the duration, as a program outside the layer would be

#### Scenario: Pinned application, same GPU back
- **WHEN** a pinned application's GPU is reset or power-cycled and the driver can reproduce the addresses
- **THEN** the application is rebuilt on it and continues
