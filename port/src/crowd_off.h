// No crowd (weak phones: Mali mode). arena_crowd = auto (off in Mali mode) /
// on / off. Off: the arena files the game opens are copies with the crowd's
// people emptied (one vertex, no triangles - the game still finds them),
// made once in the background and kept in Mods/ArenaCrowdless; the crowd's
// per-frame animation is skipped too.
#pragma once

#include <filesystem>
#include <functional>

namespace svr2011::crowd {

// Whether the crowd is off (arena_crowd, or Mali mode for auto).
bool Off();

// The file to serve for arena file `source` (BGnn.PAC): its crowd-less copy
// once made, else `source` itself - and the copy is then made in the
// background; `ready` runs (on that thread) when it is there.
std::filesystem::path Serve(const std::filesystem::path& source, const std::filesystem::path& cache_dir,
                            std::function<void()> ready);

}  // namespace svr2011::crowd
