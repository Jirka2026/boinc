# BOINC Xbox

This directory contains the Xbox Series X Developer Mode port scaffold.

## Requirements

- Visual Studio 2022
- Universal Windows Platform / UWP C++ tools
- Windows SDK
- Xbox Series X in Developer Mode
- x64 build target

Open:

`xbox/BOINC-Xbox.sln`

Build configuration:

`Debug | x64`

The first milestone only verifies that the packaged Xbox UWP application starts and can initialize the isolated BOINC Xbox platform layer.

The standard BOINC desktop Windows build remains separate and unchanged.
