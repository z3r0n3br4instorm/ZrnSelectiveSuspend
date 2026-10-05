## Purpose

Lets a running Vulkan application keep working when the GPU it renders on is taken away in an orderly fashion, by moving its graphics state to another GPU or parking it until a suitable GPU returns.

## ADDED Requirements

### Requirement: Applications opt in at start
An application SHALL be migratable only if it was started with the ZSS layer active. The layer SHALL register the application with the daemon, identifying the process and the GPU it renders on, before the application submits its first frame. Under the layer an application SHALL see a Vulkan 1.0 device.

#### Scenario: Application started under the layer
- **WHEN** an application starts with the ZSS layer active and creates a device on a GPU
- **THEN** the daemon lists that process as a migratable client of that GPU

#### Scenario: Application started without the layer
- **WHEN** an application starts without the ZSS layer and opens a GPU
- **THEN** the daemon lists that process as a non-migratable client of that GPU

#### Scenario: Device version under the layer
- **WHEN** an application started with the ZSS layer active queries a device's Vulkan version
- **THEN** the version reported is 1.0

#### Scenario: Daemon not running
- **WHEN** an application starts with the ZSS layer active and the daemon is not reachable
- **THEN** the application runs normally and is not migratable

### Requirement: Migration preserves application state
On a migrate request, the layer SHALL move the application's graphics state from its current GPU to the named target GPU without restarting the process. After migration the application SHALL continue rendering, and content it created before the migration, including content generated on the GPU, SHALL be present on the target.

#### Scenario: Application migrates and keeps rendering
- **WHEN** the daemon asks a registered application to migrate to another GPU
- **THEN** the application presents new frames from the target GPU and the process ID is unchanged

#### Scenario: GPU-generated content survives
- **WHEN** an application has rendered into an image that it reuses across frames, and is then migrated
- **THEN** the image holds the same contents on the target GPU as it did before migration

#### Scenario: Source GPU is fully released
- **WHEN** a migration completes
- **THEN** the application holds no open handle to the source GPU

#### Scenario: Windowed application
- **WHEN** an application presenting to a window is migrated
- **THEN** its swapchain reports out-of-date, and after the application recreates it presentation continues from the target GPU

### Requirement: Migration outcome is reported
The layer SHALL report exactly one outcome to the daemon for every migrate request: migrated, parked, or failed with a reason.

#### Scenario: Successful migration is acknowledged
- **WHEN** a migration completes
- **THEN** the daemon receives a migrated outcome naming the target GPU

#### Scenario: Migration fails
- **WHEN** the target GPU rejects a resource the application needs during migration
- **THEN** the application remains running on its original GPU and the daemon receives a failed outcome with the reason

### Requirement: Untracked usage is declared non-migratable
When an application uses a Vulkan feature the layer does not track, the layer SHALL mark that application non-migratable and tell the daemon which feature caused it. The layer SHALL NOT attempt to migrate such an application.

#### Scenario: Unsupported feature used
- **WHEN** a registered application creates an object of a kind the layer does not track
- **THEN** the daemon lists the application as non-migratable with the feature named

### Requirement: Incompatible targets cause parking
When the target GPU lacks a capability the application has enabled or relies on, or when no target GPU exists, the layer SHALL park the application instead of migrating it: its graphics state is captured in system memory, its handles to the source GPU are closed, and its rendering is suspended.

#### Scenario: Target GPU is weaker
- **WHEN** an application that enabled a feature absent on the target GPU is asked to migrate
- **THEN** the application is parked, the source GPU is released, and the daemon receives a parked outcome naming the missing capability

#### Scenario: No other GPU exists
- **WHEN** an application is asked to leave its GPU and no other GPU is available
- **THEN** the application is parked and the daemon receives a parked outcome

### Requirement: Parked applications resume on a suitable GPU
A parked application SHALL resume, with its captured state restored, when the daemon asks it to resume on a GPU that satisfies its capabilities. If no such GPU is available the resume SHALL fail with the error `ZSSFailedResumeNoDRM` and the application SHALL remain parked.

#### Scenario: Original GPU returns
- **WHEN** a parked application is asked to resume on its original GPU after that GPU is re-attached
- **THEN** the application resumes rendering with its earlier contents intact

#### Scenario: Resume requested with no suitable GPU
- **WHEN** a resume is requested for a parked application while no suitable GPU is available
- **THEN** the request fails with `ZSSFailedResumeNoDRM` and the application stays parked

### Requirement: Applications return to their origin GPU
The layer SHALL remember the GPU each application originally rendered on, so that a migrated application can be moved back when that GPU returns.

#### Scenario: Migrate back after re-attach
- **WHEN** an application that was migrated away is asked to migrate back to its origin GPU
- **THEN** the application presents new frames from the origin GPU with its contents intact
