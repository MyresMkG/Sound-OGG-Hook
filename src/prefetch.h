// Background prefetch: find the Ogg files the game could load and decode them
// before anything asks.
//
// Files are located on disk rather than through the engine, because the engine
// gives us no cheap way to read a file by name, and because the sources are
// known: the game's own folder (plus its dlc subfolders), the user's
// `Documents\Paradox Interactive\Hearts of Iron IV\mod` folder, and the Steam Workshop
// folder for appid 394360. Mods are loose folders in all three cases; a mod
// packed as a .zip is simply not prefetched and falls back to decoding on
// demand.
//
// Nothing here is authoritative: the hook always verifies the decoded data
// against the stream the game actually opened (content fingerprint), so a wrong
// guess costs a synchronous decode, never a wrong sound.
#pragma once

#include "cache.h"
#include "config.h"

namespace oggsound {

// Spawns the scanner and the worker pool. Returns immediately; a no-op when
// prefetching is disabled in the config.
void StartPrefetch(const Config& config, Cache* cache);

}  // namespace oggsound
