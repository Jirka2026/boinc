# BOINC Xbox Series X port

Development branch: `xbox-series-x`

## Goal

Port the BOINC core client to Xbox Series X running in Developer Mode without changing the normal Windows desktop client.

## Design rule

Xbox-specific code lives behind the `BOINC_XBOX` compile definition. Desktop Windows behavior must remain unchanged.

## First blockers identified

1. The desktop client launches science applications as separate Win32 processes.
2. Windows sandbox accounts and access tokens are used by the desktop implementation.
3. The Windows client uses Win32 shared memory and process-management APIs.
4. Host detection contains desktop Windows APIs that need an Xbox-compatible implementation.
5. The existing Visual Studio project links desktop-only Windows libraries.

## Initial milestones

- [x] Create dedicated development branch.
- [x] Add isolated Xbox platform scaffold.
- [ ] Add a Visual Studio Xbox/UWP x64 project.
- [ ] Build the BOINC protocol/network core without task launching.
- [ ] Implement Xbox-safe storage paths.
- [ ] Implement Xbox host information.
- [ ] Connect to a BOINC server and fetch project metadata.
- [ ] Add an in-package CPU worker model for a first sample application.
- [ ] Add checkpoint/resume handling.
- [ ] Validate on Xbox Series X Developer Mode.

## Important architectural constraint

The first Xbox version will not attempt to execute arbitrary downloaded Win32 executables. Science applications will need an Xbox-compatible packaged worker or another execution model supported by the Xbox sandbox.
