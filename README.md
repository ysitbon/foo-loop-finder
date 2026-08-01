# Loop Finder for foobar2000

`foo_loop_finder` is a beat-grid loop editor for foobar2000 v2 x64. It is
designed for finding musical loops visually and, later, auditioning synchronized
drum patterns over them.

## Current scope

- Display-ready waveform peak/RMS reduction
- Manual BPM and rhythmic grid calculation
- Configurable grid offset and subdivisions
- Snapped IN/OUT points
- Opt-in loop transport, always disabled on startup
- Platform-independent core with unit tests
- Installable x64 foobar2000 component registration

This milestone registers **Loop Finder 0.1.0** so it appears in
**Preferences > Components**. It does not provide a visible panel or loop
controls yet. The loop remains disabled by default.

## Windows build

The build requires Windows, Visual Studio Build Tools 2022 with the **Desktop
development with C++** workload, and the official foobar2000 SDK `2025-03-07`.
The Visual Studio IDE, WTL, and ATL are not required.

Extract the SDK contents into `external\foobar2000-sdk` so the layout is:

```text
external\foobar2000-sdk\
├── foobar2000\
│   ├── SDK\
│   ├── shared\
│   └── foobar2000_component_client\
└── pfc\
```

From a regular PowerShell terminal in the repository root, run:

```powershell
.\build.ps1 -Clean
```

To keep the SDK elsewhere, pass its extracted root:

```powershell
.\build.ps1 -Clean -FoobarSdkPath "D:\path\to\SDK-2025-03-07"
```

The script discovers and initializes Build Tools, builds and tests the CMake
core in the generator-specific `build\vs2022-x64` directory, builds the native
MSBuild project, and creates these installable outputs:

```text
build\native\Release\foo_loop_finder.dll
build\foo_loop_finder.fb2k-component
```

Install the package using **foobar2000 → File → Preferences → Components →
Install**, apply the change, and restart when prompted. The package is a
ZIP-compatible archive with `foo_loop_finder.dll` at its root.

See [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) for requirements,
troubleshooting, direct MSBuild usage, and all artifact paths.

## SDK-independent core build

The platform-independent library and tests can still be built without the
foobar2000 SDK:

```powershell
cmake -S . -B build\core
cmake --build build\core
ctest --test-dir build\core --output-on-failure
```

## Roadmap

1. Default UI panel with waveform, BPM, offset, IN/OUT and Loop controls
2. Decode the current track in the background and cache its waveform
3. Connect playback callbacks and seek from OUT to IN
4. Add tap tempo and per-track persistence
5. Add automatic BPM/beat detection
6. Add synchronized Boom bap, Funk, Rock and Jazz drum DSP
