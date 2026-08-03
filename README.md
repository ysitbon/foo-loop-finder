# Loop Finder for foobar2000

`foo_loop_finder` is a beat-grid loop editor for foobar2000 v2 x64. It is
designed for finding musical loops visually and, later, auditioning synchronized
drum patterns over them.

## Current scope

- Display-ready waveform peak/RMS reduction
- Manual BPM and rhythmic grid calculation
- Configurable grid offset and subdivisions
- Free or beat-division snapping with draggable IN/OUT points
- Editable 20-300 BPM and robust four-tap tempo entry
- Beat, subdivision and emphasized bar overlays
- Per-track grid and marker persistence
- Opt-in editor Loop state, always disabled on startup and track changes
- Platform-independent core with unit tests
- Installable x64 foobar2000 component registration
- Default UI panel with current title, playback state and entire-track waveform
- Smooth playback cursor, click-to-seek, wheel zoom and drag navigation
- Cancellable background decoding and an eight-track in-memory waveform cache

The native component registers **Loop Finder 0.1.0** and provides a **Loop
Finder** Default UI element. The panel displays the current title,
Playing/Paused/Stopped state, analysis status, waveform and playback cursor,
plus the M4 rhythmic editor. The visible Loop control arms editor state only;
audible OUT-to-IN playback starts in M5 and is intentionally absent here.

Editor controls:

- Enter BPM from 20 through 300 (decimals such as `92.5` are supported).
- Press **Tap** or **Ctrl+T** four or more times for rolling-median tap tempo.
- Enter grid phase in milliseconds; negative finite offsets are supported.
- Select free placement, 1 beat, 1/2, 1/4, 1/8 or 1/16 beat snapping.
- Use **Set IN** / **Set OUT** at the playback cursor or drag marker lines.
- Inspect loop time, seconds, beats and meaningful whole-bar duration.
- Arm **Loop (editor only)** without changing playback behavior.

Waveform interactions:

- Click the waveform to seek when foobar2000 reports the track as seekable.
- Turn the mouse wheel over the waveform to zoom around the pointer, up to 64x.
- Drag horizontally to navigate a zoomed view.
- Drag an IN or OUT marker line to edit it; marker hits take priority over pan.
- Double-click to restore the whole-track overview.

The static waveform is rendered to one off-screen layer. The grid is composed
into a separate cached presentation layer, while the two lightweight marker
lines and playback cursor are drawn over that cache. Marker edits therefore do
not rebuild the grid, waveform analysis or waveform bitmap. During ordinary
playback, redraws occur only when the cursor crosses a display pixel and affect
only its old and new strips. Analysis format `waveform-v2` retains up to 262,144
bins so zoomed views do not have to enlarge low-resolution overview data.

When a pan rebuild is needed, the last completed layer remains visible until
the replacement is ready and is then copied to the panel in one operation.

Analysis is intentionally limited to decoder-supported local tracks with a
positive known duration. Remote, unsupported, zero-length and unknown-length
sources show **Analysis unavailable** and do not affect playback. The waveform
cache is memory-only and is cleared when the panel/component session ends.
Editor metadata is stored separately through foobar2000's per-track index,
keyed by path and subsong. It never writes audio tags or saves the active Loop
flag; orphaned records expire after 26 weeks.

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

To show the panel, enable Default UI layout editing, add **Loop Finder** from
the **Playback Information** group, and disable layout editing again.

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

1. Manually accept the M4 editor in foobar2000
2. Connect playback callbacks and seek from OUT to IN for M5
3. Add automatic BPM/beat detection
4. Add synchronized Boom bap, Funk, Rock and Jazz drum DSP
