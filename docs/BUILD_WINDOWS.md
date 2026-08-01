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

## Native adapter

The official SDK is distributed as a Visual Studio solution. Extract it next
to this repository, add `src/foobar/component.cpp` and the three `src/core/*.cpp`
files to a copy of the SDK `foo_sample` component, then:

1. Rename the output project and DLL to `foo_loop_finder`.
2. Add this repository's `include` directory to **Additional Include Directories**.
3. Select `Release | x64`.
4. Build and package `foo_loop_finder.dll` as an `.fb2k-component` archive.

The adapter currently registers the component only. The next milestone adds the
Default UI element and connects playback callbacks to `LoopEngine`.

