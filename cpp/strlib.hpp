#pragma once
// HTML-specific set and suggestion helpers; common byte operations live in the SDK.
// No lexbor types here.
#include <string>
#include <cstdint>
#include <vector>

namespace lmhtml::strlib {

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
