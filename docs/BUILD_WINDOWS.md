# Building the foobar2000 component on Windows

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2022 with **Desktop development with C++**
- foobar2000 v2 x64
- foobar2000 SDK `2025-03-07`

## Core library

The audio-independent core can be built immediately:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This build produces the core library and the test executable. Depending on the
generator, the relevant outputs will look like:

```text
loop_finder_core.lib
loop_finder_tests.exe
```

Neither file can be loaded by foobar2000. A foobar2000 component must be a DLL
built against the foobar2000 SDK.

## Native adapter

The official SDK is distributed as a Visual Studio solution. Extract it next
to this repository, add `src/foobar/component.cpp` and the three `src/core/*.cpp`
files to a copy of the SDK `foo_sample` component, then:

1. Rename the output project and DLL to `foo_loop_finder`.
2. Add this repository's `include` directory to **Additional Include Directories**.
3. Select `Release | x64`.
4. Build the project and confirm that it produces `foo_loop_finder.dll`.

The component architecture must match the player architecture. The instructions
above target foobar2000 v2 x64 and therefore require a `Release | x64` DLL.

## Package the component

From PowerShell, create an installation package containing the DLL at the root
of the archive:

```powershell
New-Item package -ItemType Directory -Force
Copy-Item path\to\foo_loop_finder.dll package\
Compress-Archive package\* foo_loop_finder.zip -Force
Rename-Item foo_loop_finder.zip foo_loop_finder.fb2k-component
```

## Install in foobar2000

1. Open **File > Preferences > Components**.
2. Click **Install...**.
3. Select `foo_loop_finder.fb2k-component`.
4. Click **Apply** and restart foobar2000 when prompted.
5. Return to **Preferences > Components** and verify that **Loop Finder 0.1.0**
   appears in the installed component list.

You can also open the `.fb2k-component` file directly and let foobar2000 handle
the installation.

## Current limitation

The adapter currently registers the component only. The next milestone adds the
Default UI element and connects playback callbacks to `LoopEngine`. Therefore,
successfully installing version 0.1.0 only makes it appear in the installed
component list: it does not add a visible panel or loop controls yet.

If the build directory contains only `loop_finder_core.lib` and
`loop_finder_tests.exe`, the native SDK component has not been built yet.
