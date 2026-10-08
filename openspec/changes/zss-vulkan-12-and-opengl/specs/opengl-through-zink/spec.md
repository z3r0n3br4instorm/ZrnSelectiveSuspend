## Purpose

Lets OpenGL and OpenGL ES programs run under the layer, and so be moved and recovered, by running them on Mesa's Zink driver.

## ADDED Requirements

### Requirement: An OpenGL program can be started under the layer
The launcher SHALL offer a mode that runs an OpenGL or OpenGL ES program on Zink with the layer as Zink's Vulkan driver, without the program being changed.

#### Scenario: Launch
- **WHEN** a GLX program is started with the launcher's OpenGL mode
- **THEN** it renders, its renderer string names Zink, and it is listed by the daemon as an application under the layer

#### Scenario: EGL program
- **WHEN** an EGL program is started the same way
- **THEN** it renders through Zink and is listed likewise

#### Scenario: Zink unavailable
- **WHEN** Zink is not installed
- **THEN** the launcher says so and does not start the program on another driver unnoticed

### Requirement: The layer offers what Zink needs for current OpenGL
The layer SHALL present the Vulkan features and extensions Zink requires to expose at least desktop OpenGL 3.3 and OpenGL ES 3.0, on GPUs whose drivers have them.

#### Scenario: Version reached
- **WHEN** an OpenGL program queries its context under the launcher's OpenGL mode on a capable GPU
- **THEN** the context reports OpenGL 3.3 or higher

### Requirement: An OpenGL program migrates
An OpenGL program running on Zink under the layer SHALL be moved to another GPU on request and keep rendering, and SHALL be recovered after a loss, like a Vulkan application.

#### Scenario: Move and return
- **WHEN** the GPU an OpenGL program is on is detached and later attached again
- **THEN** the program keeps rendering throughout and ends on the GPU it started on

#### Scenario: Picture after a move
- **WHEN** an OpenGL program is moved between two GPUs
- **THEN** its picture afterwards matches the picture before within the tolerance used for moves between different drivers
