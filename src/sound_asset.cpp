#include "sound_asset.h"

#include <algorithm>

namespace oggsound {
namespace {

// Nesting is a handful of levels deep in every asset seen so far; the cap only
// protects the recursion against a malformed file.
constexpr int kMaxDepth = 24;

bool IsNameStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9'); }

bool IsSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool NameEquals(const std::string& text, size_t begin, size_t end, const char* lower) {
  size_t i = 0;
  for (size_t at = begin; at < end; ++at, ++i) {
    char c = text[at];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (lower[i] == '\0' || c != lower[i]) return false;
  }
  return lower[i] == '\0';
}

// `#` to the end of the line, leaving quoted strings alone.
std::string StripComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool in_string = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      out.push_back(c);
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
      out.push_back(c);
      continue;
    }
    if (c == '#') {
      while (i < text.size() && text[i] != '\n') ++i;
      out.push_back('\n');
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// Offset of the '}' closing the '{' at |open|, or npos.
size_t MatchBrace(const std::string& text, size_t open, size_t end) {
  int depth = 0;
  bool in_string = false;
  for (size_t i = open; i < end; ++i) {
    const char c = text[i];
    if (in_string) {
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '{') {
      ++depth;
      continue;
    }
    if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

void ScanLevel(const std::string& text, size_t begin, size_t end, int depth,
               std::vector<std::string>* out, const char* key) {
  size_t i = begin;
  while (i < end) {
    if (!IsNameStart(text[i])) {
      ++i;
      continue;
    }
    size_t name_end = i;
    while (name_end < end && IsNameChar(text[name_end])) ++name_end;

    size_t cursor = name_end;
    while (cursor < end && IsSpaceChar(text[cursor])) ++cursor;
    if (cursor >= end || text[cursor] != '=') {
      i = name_end;
      continue;
    }
    ++cursor;
    while (cursor < end && IsSpaceChar(text[cursor])) ++cursor;
    if (cursor >= end) return;

    if (text[cursor] == '{') {
      const size_t close = MatchBrace(text, cursor, end);
      if (close == std::string::npos) return;  // malformed: nothing more to read
      if (depth < kMaxDepth && !NameEquals(text, i, name_end, "music")) {
        ScanLevel(text, cursor + 1, close, depth + 1, out, key);
      }
      i = close + 1;
      continue;
    }

    if (NameEquals(text, i, name_end, key) && text[cursor] == '"') {
      const size_t start = cursor + 1;
      size_t stop = start;
      while (stop < end && text[stop] != '"') ++stop;
      if (stop >= end) return;
      out->push_back(text.substr(start, stop - start));
      i = stop + 1;
      continue;
    }
    i = name_end;
  }
}

}  // namespace

std::wstring JoinPath(const std::wstring& dir, const wchar_t* leaf) {
  if (dir.empty()) return std::wstring(leaf);
  if (leaf == nullptr || leaf[0] == 0) return dir;
  if (dir.back() == L'\\' || dir.back() == L'/') return dir + leaf;
  return dir + L"\\" + leaf;
}

std::vector<std::string> AssetFileEntries(const std::string& text) {
  std::vector<std::string> out;
  const std::string clean = StripComments(text);
  ScanLevel(clean, 0, clean.size(), 0, &out, "file");
  return out;
}

std::vector<std::string> ModDescriptorPaths(const std::string& text) {
  std::vector<std::string> out;
  const std::string clean = StripComments(text);
  ScanLevel(clean, 0, clean.size(), 0, &out, "path");
  return out;
}

std::vector<std::wstring> AssetEntryCandidates(const std::wstring& root,
                                               const std::wstring& asset_dir,
                                               const std::wstring& entry) {
  std::vector<std::wstring> out;
  std::wstring relative = entry;
  std::replace(relative.begin(), relative.end(), L'/', L'\\');
  size_t begin = 0;
  while (begin < relative.size() && relative[begin] == L'\\') ++begin;
  relative.erase(0, begin);
  if (relative.empty()) return out;

  const std::wstring bases[] = {root, JoinPath(root, L"sound"), asset_dir};
  for (const std::wstring& base : bases) {
    const std::wstring candidate = JoinPath(base, relative.c_str());
    if (candidate.empty()) continue;
    bool duplicate = false;
    for (const std::wstring& existing : out) {
      if (existing == candidate) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) out.push_back(candidate);
  }
  return out;
}

}  // namespace oggsound
