# Roadmap

This file is the source of truth for implementation status. A checkbox is
completed only when its acceptance evidence has been observed. Code presence
alone is not proof that native integration works.

## M0 — Portable core

- [x] Define validated `LoopState` and `LoopEngine`.
- [x] Keep Loop disabled on engine construction.
- [x] Calculate beat duration and rhythmic grid lines.
- [x] Snap IN/OUT markers to configurable subdivisions.
- [x] Reduce interleaved PCM to waveform min/max/RMS bins.
- [x] Cover the core behavior with unit tests.
- [x] Build the core and pass tests with C++20.

Acceptance evidence: the existing core test executable reports all tests
passing on Linux and Windows/MSVC.

## M1 — Loadable foobar2000 component

- [x] Declare the `Loop Finder 0.1.0` component identity.
- [ ] Provide a Build Tools-only native x64 project linked to SDK 2025-03-07.
- [ ] Produce `build\native\Release\foo_loop_finder.dll`.
- [ ] Package the DLL at the archive root of
      `build\foo_loop_finder.fb2k-component`.
- [ ] Install the package in foobar2000 v2 x64.
- [ ] Verify `Loop Finder 0.1.0` appears under Preferences > Components.

Acceptance evidence: retain the successful Windows build output, inspect the
package contents and manually verify the installed component entry. Do not mark
this milestone complete based only on a `.vcxproj` or registration source.

## M2 — Default UI panel shell

- [ ] Register a Default UI element.
- [ ] Render a placeholder panel in foobar2000.
- [ ] Follow foobar2000 light/dark colors and DPI scaling.
- [ ] Show track title and basic playback state.
- [ ] Keep UI updates on the correct thread.

Acceptance evidence: add the panel to a Default UI layout, restart foobar2000
and verify it renders without crashes or startup regressions.

## M3 — Waveform view

- [ ] Decode the current local track in a background task.
- [ ] Feed decoded PCM into the portable waveform reducer.
- [ ] Render the entire-track waveform.
- [ ] Render and update the playback cursor.
- [ ] Add click-to-seek, zoom and horizontal navigation.
- [ ] Cancel stale analysis when the current track changes.
- [ ] Cache waveform data by stable track identity and analysis version.

Acceptance evidence: switch rapidly between tracks, seek and restart foobar2000
without blocking playback, showing stale data or crashing.

## M4 — Manual rhythmic loop editor

- [ ] Add editable BPM with validation from 20 to 300.
- [ ] Add tap tempo.
- [ ] Render beat and emphasized bar grid lines.
- [ ] Add adjustable grid phase/offset.
- [ ] Add snapping divisions and a free-placement mode.
- [ ] Add draggable IN and OUT markers.
- [ ] Display loop duration in time, beats and bars.
- [ ] Add a Loop toggle that is disabled by default.
- [ ] Persist per-track grid and marker state without persisting active Loop.

Acceptance evidence: edit a loop visually, reload the track and confirm markers
return while Loop remains disabled.

## M5 — Playback looping

- [ ] Observe playback position through the foobar2000 SDK.
- [ ] Seek from OUT to IN only while Loop is enabled.
- [ ] Do not intercept manual seeking.
- [ ] Prevent recursive or duplicate automatic seeks.
- [ ] Handle pause, stop, track change and invalid/unseekable sources safely.
- [ ] Measure boundary jitter and document the initial transport limitation.

Acceptance evidence: repeatedly loop short and multi-bar regions, manually seek
inside and outside them, and change tracks without transport lockups.

## M6 — Automatic tempo analysis

- [ ] Evaluate the BPM/beat library and its redistribution licence.
- [ ] Analyze BPM and beat positions outside the audio thread.
- [ ] Report confidence and allow half/double-time correction.
- [ ] Allow manual BPM and phase to override detection.
- [ ] Cache automatic analysis separately from user overrides.

Acceptance evidence: test straight, syncopated, half-time and variable-intro
material; automatic analysis must never prevent manual correction.

## M7 — Click-free looping

- [ ] Design buffered playback without violating foobar2000 DSP/thread rules.
- [ ] Add a short configurable boundary crossfade.
- [ ] Avoid clicks at non-zero crossings.
- [ ] Quantify and document achievable loop accuracy.

Acceptance evidence: record or inspect repeated boundaries across representative
sample rates and loop lengths without audible discontinuities.

## M8 — Synchronized drum overlay

- [ ] Define a sequencer/DSP boundary independent from `LoopEngine`.
- [ ] Add independent drum enable and volume controls.
- [ ] Synchronize drums to grid BPM and phase.
- [ ] Add Boom bap, Funk, Rock and Jazz patterns.
- [ ] Add kit selection and optional swing.
- [ ] Confirm every bundled sample is original, CC0 or redistributable.
- [ ] Keep drum and track-loop activation independent.

Acceptance evidence: change tempo and phase while auditioning each style without
drift, audio-thread allocation spikes or implicit Loop activation.

## Current next action

Complete M1 by implementing and actually running the Build Tools-only native
build against the official SDK, then install the generated package in
foobar2000. After M1 is verified, begin M2 rather than jumping directly to
waveform or playback behavior.
