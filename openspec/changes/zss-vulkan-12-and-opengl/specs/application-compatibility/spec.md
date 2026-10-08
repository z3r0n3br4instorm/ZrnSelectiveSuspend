## Purpose

Keeps the work pointed at real applications: measures what they need, names the ones that must work, and looks after programs that still run outside the layer.

## ADDED Requirements

### Requirement: What an application asks of Vulkan can be recorded
The project SHALL provide a tool that runs a program on the real drivers and reports the Vulkan version it requests, the extensions and features it enables, the commands it calls and the structures it passes.

#### Scenario: Survey
- **WHEN** a program is run under the survey tool and exits
- **THEN** a report is written listing its requested version, enabled extensions, enabled features, and each command called with a count

#### Scenario: Something the layer lacks
- **WHEN** a surveyed program uses a command or extension the layer does not offer
- **THEN** the report marks it as missing

### Requirement: Named applications run and move under the layer
The following SHALL start under the layer, render correctly, move to another GPU and back on request, and survive an injected loss: `vkcube` at Vulkan 1.2, `glxgears` and `glmark2` through Zink, the Zed editor, and Firefox.

#### Scenario: Zed
- **WHEN** Zed is started under the layer with a file open, its GPU is detached and attached again
- **THEN** the editor stays usable throughout and shows the file correctly afterwards

#### Scenario: Firefox
- **WHEN** Firefox is started under the layer's OpenGL mode on a page with animated content, its GPU is detached and attached again
- **THEN** the page keeps animating and renders correctly afterwards

#### Scenario: Application that cannot be made to work
- **WHEN** one of the named applications cannot run or cannot move on the reference hardware
- **THEN** the reason is recorded with the survey that shows it, and the application is listed as not working, not left out

### Requirement: A frozen program keeps its picture
A program that runs outside the layer and is frozen for a power-off SHALL show the same picture after the GPU is powered on again as before.

#### Scenario: OpenGL program on the vendor driver
- **WHEN** a program rendering through the NVIDIA driver is frozen for a power-off and the GPU is powered on again
- **THEN** it continues with correct colours and contents
