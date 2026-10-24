#pragma once
// String helpers bound as ordinary schema functions (str_*). Kept separate
// from the HTML host so the set can later move into a lmcode-cpp catalog.
// No lexbor types here.
#include <string>
#include <cstdint>
#include <vector>

namespace lmhtml::strlib {

bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);
bool contains(const std::string &s, const std::string &needle);
std::string lower(const std::string &s);
// start is clamped to [0, size]; length to the remaining bytes. Byte-based.
std::string substr(const std::string &s, int64_t start, int64_t length);
int64_t len(const std::string &s); // bytes

// Glob match: `*` any run (including /), `?` one byte, `[...]` byte class
// with ranges and `[!...]` negation (`]` first is literal; an unterminated
// `[` is a literal '['), `\x` a literal x. Bytes, not code points: `?`
// matches one byte of a multi-byte UTF-8 character. Worst case
// O(|text|*|pattern|), capped at 2^27 steps (throws std::string beyond).
bool match(const std::string &text, const std::string &pattern);

// Items of a that are not in b; order of a kept, duplicates removed.
// Host-side hash set: scripts never loop two collections O(n^2).
std::vector<std::string> difference(const std::vector<std::string> &a,
                                    const std::vector<std::string> &b);

// Union of a and b; order of a then new items of b, duplicates removed.
std::vector<std::string> set_union(const std::vector<std::string> &a,
                                   const std::vector<std::string> &b);

// Near-miss suggestions for name among candidates: Damerau-Levenshtein
// (OSA) distance <= max(1, len/4), sorted by distance then alphabetically.
std::vector<std::string> suggest(const std::string &name,
                                 const std::vector<std::string> &candidates);

}
