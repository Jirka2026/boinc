# Building BOINC Xbox

## Visual Studio requirements

Use Visual Studio 2022 with the UWP C++ toolchain installed.

Required components:

- Universal Windows Platform development / UWP tools
- C++ Universal Windows Platform tools for the installed v143 toolset
- Windows 10 or Windows 11 SDK
- MSVC v143 x64 build tools

Microsoft documents the UWP C++ toolchain as the supported route for UWP development, and the Xbox UWP vcpkg target is x64-uwp.

## Open the solution

Open:

`xbox/BOINC-Xbox.sln`

Select:

- Configuration: `Debug`
- Platform: `x64`

## First milestone

The first build intentionally contains only:

- the packaged UWP application shell
- Xbox-specific package manifest
- internet/private-network capabilities
- the isolated BOINC Xbox platform layer

It does not yet compile the full BOINC core client and it does not launch downloaded science executables.

## Xbox deployment

The console must be in Developer Mode.

Visual Studio can deploy UWP applications to an Xbox in Developer Mode after the console is paired as a remote target. The package manifest targets the `Windows.Xbox` device family.

If Visual Studio requests package signing when creating or installing an MSIX/AppX package, create/select a test certificate whose subject matches the Publisher value in `Package.appxmanifest`. Package signing is required for installable MSIX/AppX packages.

## Expected first screen

The app should display:

- BOINC Xbox
- Xbox Series X Developer Mode port
- x86_64-pc-xbox-uwp
- Xbox platform scaffold initialized
