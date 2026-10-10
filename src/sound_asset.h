// Reading HOI4 sound assets: which files they name, and where those files
// live on disk.
//
// The prefetcher uses this to decide what to decode ahead of time. Assets that
// sit under a `sound\` folder are read, and everything in them is taken except
// `music = { ... }` blocks: the music player goes through libvorbisfile, never
// through the sound loader this DLL hooks, so a decoded music track would sit in
// the cache and could never be handed out.
#pragma once

#include <string>
#include <vector>

namespace oggsound {

// |dir| joined with |leaf|, tolerating a separator at the end of |dir|.
std::wstring JoinPath(const std::wstring& dir, const wchar_t* leaf);

// Every `file = "..."` value in an asset, in order of appearance, skipping the
// ones inside a `music = { ... }` block. `#` comments and quoted strings are
// honoured, and block nesting is followed, so entries written under
// `category = { soundeffects = { sound = { ... } } }` are found too.
std::vector<std::string> AssetFileEntries(const std::string& text);

// Folder paths from a launcher .mod descriptor, including external local mods.
std::vector<std::string> ModDescriptorPaths(const std::string& text);

// The paths one entry may denote, in the order the engine would try them:
//   1. <root>/<entry>          - entries written as "sound/xxx.ogg"
//   2. <root>/sound/<entry>    - the usual convention, relative to sound/
//   3. <asset_dir>/<entry>     - entries written next to the asset itself
// A leading separator is ignored and '/' counts as a path separator.
std::vector<std::wstring> AssetEntryCandidates(const std::wstring& root,
                                               const std::wstring& asset_dir,
                                               const std::wstring& entry);

}  // namespace oggsound
