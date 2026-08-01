# Loop Finder agent instructions

## Mission

Build `foo_loop_finder`, a foobar2000 v2 component for visually finding,
editing and auditioning musical loops. The MVP displays the current track as a
waveform with a tempo grid, draggable IN/OUT markers and an opt-in Loop control.
Later milestones add beat detection and synchronized drum patterns.

Treat [docs/ROADMAP.md](docs/ROADMAP.md) as the source of truth for project
status. Continue from its first incomplete milestone unless the user explicitly
requests different work. Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before
changing module boundaries or audio behavior. Use
[docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) for native build instructions.

## Product invariants

- Loop playback is always disabled on application and component startup.
- Restoring saved IN/OUT points must never implicitly enable looping.
- IN must be non-negative and OUT must be strictly after IN.
- Invalid BPM, marker or grid input must not mutate the last valid state.
- Manual seeking must not be intercepted as an automatic loop transition.
- UI and analysis work must not block foobar2000's audio or main thread.
- A component architecture must match the foobar2000 architecture; the initial
  supported target is foobar2000 v2 x64 on Windows 10 or 11.

## Architecture rules

- Keep `include/loop_finder` and `src/core` independent of foobar2000, Win32 and
  UI frameworks.
- Keep foobar2000 SDK calls and lifecycle adapters under `src/foobar`.
- Keep native Windows/MSBuild configuration under `native`.
- Prefer explicit state transitions in `LoopEngine` over UI-owned behavior.
- Do not put automatic BPM detection or drum sequencing into `LoopEngine`.
- Do not introduce WTL, ATL, Columns UI or another UI dependency until the
  relevant milestone requires it.
- Keep waveform analysis independent of drawing so cached data can be reused by
  different UI implementations.

## Build and validation

For platform-independent changes, run:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with Visual Studio Build Tools 2022, prefer:

```powershell
.\build.ps1 -Clean -FoobarSdkPath "C:\path\to\SDK-2025-03-07"
```

The SDK root must contain `foobar2000` and `pfc` directories. Never vendor or
commit the foobar2000 SDK.

For native component work, validation is complete only when all of the following
are true:

1. Core tests pass.
2. `foo_loop_finder.dll` builds as Release x64.
3. `foo_loop_finder.fb2k-component` contains the DLL at its archive root.
4. foobar2000 installs the package without rejecting the component.
5. The component appears as `Loop Finder` under Preferences > Components.

Do not claim native build, installation or runtime validation when it was not
actually performed. State the exact unverified boundary and give the next
Windows command instead.

## Testing expectations

- Add or update core unit tests for every state, grid, snapping or waveform
  behavior change.
- Include boundary cases for invalid BPM, IN/OUT ordering and disabled looping.
- Add adapter-level tests where practical, but never replace an actual
  foobar2000 load check with mocks.
- Preserve passing tests while implementing later milestones.

## Working conventions

- Use C++20 for project-owned code.
- Treat warnings in project-owned code as defects; do not broadly suppress SDK
  warnings.
- Avoid unrelated formatting or dependency changes.
- Preserve user changes in a dirty worktree.
- Do not commit, push, publish a release or upload a component unless explicitly
  requested.
- Update `docs/ROADMAP.md` only after its completion evidence is available.
- Update architecture/build documentation in the same change when behavior or
  workflow changes.

## Task handoff

End implementation tasks with:

- outcome and user-visible behavior;
- files changed;
- commands and tests run;
- anything not validated and why;
- the next roadmap milestone;
- one suggested Conventional Commit message.

