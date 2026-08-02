#pragma once

#ifdef _WIN32

#include <foobar2000/SDK/foobar2000.h>

#include "loop_finder/loop_state.hpp"

#include <optional>
#include <string>

namespace loop_finder::foobar {

class EditorPersistence {
public:
    static std::optional<LoopState> load(const metadb_handle_ptr& track,
                                         std::string& warning);
    static bool save(const metadb_handle_ptr& track,
                     const LoopState& state,
                     std::string& error);
};

} // namespace loop_finder::foobar

#endif
