# Building the foobar2000 component with Build Tools 2022

The repository contains a command-line MSBuild project for a real foobar2000 v2
x64 component. A Visual Studio IDE installation is not needed.

## Requirements

- Windows 10 or 11 x64
- foobar2000 v2 x64
- Official foobar2000 SDK `2025-03-07`
- Visual Studio Build Tools 2022 with:
  - **Desktop development with C++** workload
  - MSVC v143 C++ x64/x86 build tools
  - A Windows 10 or Windows 11 SDK
  - C++ CMake tools for Windows

WTL, ATL, and the SDK helpers project are not required. The Default UI panel
uses the official `ui_element` interfaces with a project-owned raw Win32 child
window, so the Build Tools requirements remain unchanged.

## SDK layout

Download the official SDK `2025-03-07` and extract its contents. For the default
build, the SDK root must be `external\foobar2000-sdk`:

```text
external\foobar2000-sdk\
├── foobar2000\
│   ├── SDK\
│   │   └── foobar2000_SDK.vcxproj
│   ├── shared\
│   │   └── shared-x64.lib
│   └── foobar2000_component_client\
│       └── foobar2000_component_client.vcxproj
└── pfc\
    └── pfc.vcxproj
```

The SDK archive itself may have a versioned outer folder. Use the directory
that directly contains `foobar2000` and `pfc` as `FoobarSdkPath`.

## Build and test

Open a regular PowerShell terminal in the repository root. The script locates
Build Tools with `vswhere.exe` and initializes the x64 compiler environment, so
it does not need a Developer PowerShell shortcut.

With the SDK in the default location:

```powershell
.\build.ps1 -Clean
```

With an SDK extracted elsewhere:

```powershell
.\build.ps1 -Clean -FoobarSdkPath "D:\path\to\SDK-2025-03-07"
```

`Release|x64` is the default. A debug component can be built with:

```powershell
.\build.ps1 -Clean -Configuration Debug
```

The script performs these operations in order:

1. Validates the SDK projects and `shared-x64.lib`.
2. Removes generated output when `-Clean` is present, before CMake configures.
3. Configures the core in `build\vs2022-x64`, keeping this generator isolated.
4. Builds the core and test executable and runs CTest unless `-SkipTests` is set.
5. Builds `native\foo_loop_finder.vcxproj` with MSBuild and `FoobarSdkRoot`.
6. Verifies that the DLL exists, packages it, and verifies the archive entry.

Expected Release outputs are:

```text
build\vs2022-x64\Release\loop_finder_core.lib
build\vs2022-x64\Release\loop_finder_tests.exe
build\native\Release\foo_loop_finder.dll
build\foo_loop_finder.fb2k-component
```

The `.fb2k-component` file is a ZIP-compatible archive containing only
`foo_loop_finder.dll` at its root.

## Direct MSBuild invocation

Normally `build.ps1` should be used because it also tests the core and packages
the DLL. From an x64 Build Tools developer shell, the native project can be
built directly when diagnosing it:

```powershell
msbuild native\foo_loop_finder.vcxproj /m /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:FoobarSdkRoot="D:\path\to\SDK-2025-03-07"
```

The project targets toolset `v143`, uses C++20 and the SDK-compatible dynamic
MSVC runtime (`/MD` or `/MDd`), and
references:

- `pfc\pfc.vcxproj`
- `foobar2000\SDK\foobar2000_SDK.vcxproj`
- `foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj`
- `foobar2000\shared\shared-x64.lib`

It compiles `src\foobar\component.cpp`, `src\foobar\editor_persistence.cpp`,
`src\foobar\ui_element.cpp`, `src\foobar\waveform_analysis.cpp` and the
platform-independent core sources, including tap tempo.
It does not copy or modify SDK sources.

The official SDK's Win32/x64 project configurations name toolset `v142` for
Visual Studio 2019 compatibility. Passing `PlatformToolset=v143` as a global
property retargets those referenced projects in memory for Build Tools 2022;
the extracted SDK project files are not edited.

## Install

In foobar2000, use **File → Preferences → Components → Install**, select
`build\foo_loop_finder.fb2k-component`, apply the change, and restart when
prompted. Return to **Preferences → Components** and confirm that **Loop Finder
0.1.0** is listed.

After restart, enable Default UI layout editing, add **Loop Finder** from the
**Playback Information** group, then disable layout editing. The panel displays
the current track title, Playing/Paused/Stopped state, waveform analysis status,
waveform and playback cursor, and the M4 editor. **Loop (editor only)** is Off
after startup and track changes and does not cause audible looping until M5.

The waveform opens as a whole-track overview. Click to seek, use the mouse
wheel over it to zoom, drag horizontally to navigate, and double-click to reset
the overview. Seeking is ignored for tracks foobar2000 reports as unseekable.

Waveform analysis supports local tracks handled by an installed foobar2000
decoder and requires a positive known duration. Remote, unsupported,
zero-length and unknown-length sources display an unavailable/error state.
Waveforms are cached only in memory for up to eight tracks.

The documented minimum panel size is 560 by 380 logical pixels. Editor metadata
is stored in foobar2000's component-managed per-track index and never in audio
tags. Location-orphaned entries expire after 26 weeks.

## M3 manual verification

The Release build and package check do not replace this runtime test:

1. Start a supported local track that is not cached; confirm **Analyzing...**
   appears, playback stays responsive, then the entire-track waveform appears.
2. Switch rapidly between several local tracks while analysis is active;
   confirm only the current track's waveform appears and there is no crash.
3. Pause and resume; confirm the cursor stops while paused and continues
   smoothly from the correct position after resume.
4. Click several waveform positions; confirm playback seeks to the matching
   time. Also try an unseekable source and confirm the click is safely ignored.
5. Turn the wheel over the waveform to zoom, drag horizontally to navigate,
   then double-click and confirm the whole-track overview returns. Confirm
   panning does not flash a blank/background frame while rebuilding the view.
6. Resize the panel at whole-track and zoomed views; confirm the waveform
   resamples cleanly and interactions still map to the visible time range.
7. Change foobar2000 theme/colors and move between tested DPI settings; confirm
   text, waveform, cursor, spacing and hit targets remain usable.
8. Stop playback during analysis and after analysis; confirm **No track** is
   shown, cursor updates stop, and no late waveform appears.
9. Restart foobar2000; confirm startup is stable, **Loop: Off** remains, and a
   local track is analyzed again because the cache is intentionally in-memory.
10. During first-time analysis while playback runs, monitor CPU usage and UI /
    audio responsiveness; confirm no stalls, dropouts, visible idle-playback
    flicker or excessive sustained redraw activity. At maximum zoom, confirm
    the `waveform-v2` detail is materially smoother than the overview data.

## M4 manual verification

The Release build and package inspection do not replace this runtime test:

1. In **File → Preferences → Components**, uninstall the prior Loop Finder if
   necessary, install `build\foo_loop_finder.fb2k-component`, apply, and restart
   foobar2000.
2. Reopen the existing panel. If needed, enable Default UI layout editing, add
   **Loop Finder** from **Playback Information**, then disable layout editing.
3. Play a local track. Enter `20`, `300`, and `92.5` in BPM, committing each
   with Enter or by moving focus; confirm each is accepted and the grid updates.
4. Try empty text, `nan`, `inf`, `19.9`, `300.1`, and nonnumeric BPM. Confirm the
   concise inline error appears, engine/grid state remains unchanged, and Escape
   restores the last valid text.
5. Click **Tap** at a steady tempo, then repeat with **Ctrl+T** while different
   panel controls have focus. Confirm no BPM is committed before four taps, the
   estimate resists one poor tap, inactivity starts a new sequence, and the
   resulting BPM remains manually editable.
6. At a known tempo, inspect subdivision, beat and stronger bar/downbeat lines;
   confirm their musical alignment and theme contrast.
7. Enter positive and negative **Offset ms** values with millisecond precision;
   confirm the grid phase moves after commit while markers do not.
8. Select **Off / free**, **1 beat**, **1/2 beat**, **1/4 beat**, **1/8 beat**,
   and **1/16 beat**. Set or drag a marker in every mode and confirm free mode is
   exact and each division snaps as labelled; changing the selector alone must
   not move existing markers.
9. Move playback and use **Set IN** and **Set OUT**, then drag both labelled
   marker lines. Confirm `IN < OUT`, out-of-range drags clamp, rejected crossing
   drags preserve the last valid marker, and dragging does not seek playback.
10. Wheel-zoom around several positions, drag empty space to pan, click empty
    space to seek, and double-click to reset. Confirm waveform, grid, markers and
    cursor remain time-aligned and the waveform is not reanalyzed.
11. Change to a second track, make different edits, then return to the first;
    confirm BPM, phase, snapping and markers return while Loop is Off.
12. Restart foobar2000 and return to both tracks; confirm editor metadata returns
    but **Loop (editor only)** is Off.
13. Arm and disarm **Loop (editor only)** while crossing OUT; confirm its state
    changes visibly but no OUT-to-IN seek or audible loop occurs in M4.
14. Resize down to 560×380 logical pixels, change foobar colors/light-dark theme,
    test another DPI, and use Tab/Shift+Tab, Enter, Escape, Space and Ctrl+T.
    Confirm controls, focus, labels, hit targets and waveform remain usable.
15. During analysis, playback, marker dragging, zooming and panning, monitor CPU,
    playback and UI responsiveness. Confirm no stalls, dropouts, stale waveform,
    blank-frame flicker or sustained idle redraw regression.

After the initial M4 acceptance pass, focused follow-up testing should also
confirm that Escape immediately restores the last valid BPM/grid-offset text
while an edit has focus, and that continuous IN/OUT marker dragging does not
flash the cached waveform or grid. A small 50 ms playback-cursor timer pattern
may remain visible in CPU graphs while playing, but it should return to the idle
baseline when playback stops and must not impair playback or UI response.

## Common errors

- If the default SDK is absent, the script reports the exact extraction target:
  `external\foobar2000-sdk`.
- If `-FoobarSdkPath` points at the archive or its parent rather than the
  extracted SDK root, the script lists every missing required file.
- If Build Tools discovery fails, add the workload and individual components
  listed under **Requirements** using Visual Studio Installer.
- A 32-bit DLL cannot load in foobar2000 v2 x64. This project intentionally has
  only `Debug|x64` and `Release|x64` configurations.
