# Loop Finder for foobar2000

`foo_loop_finder` is a beat-grid loop editor for foobar2000 v2. It is designed
for finding musical loops visually and, later, auditioning synchronized drum
patterns over them.

## MVP scope

- Display-ready waveform peak/RMS reduction
- Manual BPM and rhythmic grid calculation
- Configurable grid offset and subdivisions
- Snapped IN/OUT points
- Opt-in loop transport, always disabled on startup
- Platform-independent core with unit tests
- foobar2000 component registration skeleton

## Roadmap

1. Default UI panel with waveform, BPM, offset, IN/OUT and Loop controls
2. Decode the current track in the background and cache its waveform
3. Connect playback callbacks and seek from OUT to IN
4. Add tap tempo and per-track persistence
5. Add automatic BPM/beat detection
6. Add synchronized Boom bap, Funk, Rock and Jazz drum DSP

The first transport implementation will use foobar2000 seeking. A later
buffered implementation can add click-free crossfades and sample-accurate loops.

## Build the tested core

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

See [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) for the native component.
