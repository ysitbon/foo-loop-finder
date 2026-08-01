// Registration-only native adapter. UI and playback services are intentionally
// deferred to later milestones; loading this DLL only registers its metadata.
#ifdef _WIN32
#include <foobar2000/SDK/foobar2000.h>

DECLARE_COMPONENT_VERSION(
    "Loop Finder",
    "0.1.0",
    "Beat-grid loop finder for foobar2000.\n"
    "MVP: manual BPM, IN/OUT markers and opt-in looping.");

VALIDATE_COMPONENT_FILENAME("foo_loop_finder.dll");
#endif
