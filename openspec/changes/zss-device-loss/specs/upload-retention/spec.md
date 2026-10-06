## Purpose

Keeps a copy of the data an application uploads to the GPU, so that after a device is lost the uploads can be restored without the application's help.

## ADDED Requirements

### Requirement: Uploaded data is retained
When an application copies data from a buffer whose contents the layer can read into an image or into a buffer it cannot read, the layer SHALL retain that data from the moment the copy is submitted for as long as the destination holds it.

#### Scenario: Texture upload through a staging buffer
- **WHEN** an application uploads a texture from a staging buffer and then destroys the staging buffer
- **THEN** the texture's data is still retained

#### Scenario: Destination overwritten by the GPU
- **WHEN** an image that received an upload is later rendered into
- **THEN** the retained upload is no longer treated as that image's contents

#### Scenario: Destination destroyed
- **WHEN** the destination of an upload is destroyed
- **THEN** its retained data is no longer held on its behalf

### Requirement: Retained data is stored on disk by content
Retained data SHALL be kept in a store on disk, addressed by its content, that persists across runs. Data already present in the store SHALL NOT be written again.

#### Scenario: Second launch
- **WHEN** an application uploads the same textures on a second launch
- **THEN** no new data is written to the store for them

#### Scenario: Same data, two destinations
- **WHEN** two images receive identical uploads
- **THEN** the store holds that data once

### Requirement: Retention does not stall the application
Writing to the store SHALL happen off the thread that submits the upload. Memory held for pending writes SHALL be bounded; when the bound is reached, further uploads SHALL proceed without being retained and their destinations SHALL be treated as having unrecoverable contents.

#### Scenario: Burst of uploads larger than the bound
- **WHEN** uploads arrive faster than the store can take them and the pending bound is reached
- **THEN** the application's submits still complete, and the affected destinations are counted as unrecoverable after a loss

### Requirement: The store is bounded and safe to trim
The store SHALL have a size limit and SHALL remove least-recently-used data beyond it. Data that a running application still depends on SHALL NOT be removed.

#### Scenario: Limit exceeded
- **WHEN** the store grows past its limit
- **THEN** data no running application depends on is removed until it is within the limit

#### Scenario: Data in use
- **WHEN** the store is trimmed while an application depends on some of its data
- **THEN** that data can still be read by that application

### Requirement: Retention can be turned off
The user SHALL be able to disable retention for a process, or keep it in memory only. With retention off, uploads are simply not recoverable after a loss.

#### Scenario: Retention disabled
- **WHEN** an application runs with retention disabled
- **THEN** nothing is written to the store, and after a loss its uploaded textures are counted as unrecoverable
