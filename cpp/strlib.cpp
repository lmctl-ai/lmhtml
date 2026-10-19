#include "strlib.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace lmhtml::strlib {

bool starts_with(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string &s, const std::string &needle) {
  return s.find(needle) != std::string::npos;
}

std::string lower(const std::string &s) {
  std::string out = s;
  for (auto &c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::string substr(const std::string &s, int64_t start, int64_t length) {
  if (start < 0) start = 0;
  if (static_cast<uint64_t>(start) >= s.size() || length <= 0) return "";
  return s.substr(static_cast<size_t>(start), static_cast<size_t>(length));
}

int64_t len(const std::string &s) { return static_cast<int64_t>(s.size()); }

namespace {
// Does the single pattern element at p (not '*') match byte c? Sets `next`
// to the index after the element. `\x` is a literal x; `[` without a closing
// `]` is a literal '[' (fnmatch); `[z-a]` is an empty range.
bool element_matches(const std::string &pat, size_t p, unsigned char c, size_t &next) {
  if (pat[p] == '\\' && p + 1 < pat.size()) {
    next = p + 2;
    return static_cast<unsigned char>(pat[p + 1]) == c;
  }
  if (pat[p] == '?') { next = p + 1; return true; }
  if (pat[p] == '[') {
    size_t q = p + 1;
    bool negate = q < pat.size() && pat[q] == '!';
    if (negate) q++;
    size_t first = q;
    bool hit = false;
    while (q < pat.size() && (pat[q] != ']' || q == first)) {
      unsigned char lo = static_cast<unsigned char>(pat[q]);
      if (q + 2 < pat.size() && pat[q + 1] == '-' && pat[q + 2] != ']') {
        unsigned char hi = static_cast<unsigned char>(pat[q + 2]);
        if (c >= lo && c <= hi) hit = true;
        q += 3;
      } else {
        if (lo == c) hit = true;
        q++;
      }
    }
    if (q < pat.size()) { // terminated class
      next = q + 1;
      return hit != negate;
    }
  }
  next = p + 1;
  return static_cast<unsigned char>(pat[p]) == c;
}
} // namespace

// Wildcard matching with one backtrack point (the last '*'): O(|text| *
// |pattern|) worst case, never exponential. The step budget turns the
// polynomial worst case (1 MiB text x 32 KiB pattern took 40 s) into a clean
// error instead of a stall.
bool match(const std::string &text, const std::string &pattern) {
  const uint64_t budget = uint64_t(1) << 27;
  uint64_t steps = 0;
  size_t t = 0, p = 0, star_p = std::string::npos, star_t = 0;
  while (t < text.size()) {
    if (++steps > budget)
      throw std::string("str_match: pattern too costly for this text (more than 2^27 steps)");
    size_t next = 0;
    if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++;
      star_t = t;
    } else if (p < pattern.size() &&
               element_matches(pattern, p, static_cast<unsigned char>(text[t]), next)) {
      p = next;
      t++;
    } else if (star_p != std::string::npos) {
      p = star_p + 1;
      t = ++star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') p++;
  return p == pattern.size();
}

std::vector<std::string> difference(const std::vector<std::string> &a,
                                    const std::vector<std::string> &b) {
  std::unordered_set<std::string> excluded(b.begin(), b.end());
  std::unordered_set<std::string> seen;
  std::vector<std::string> out;
  for (const auto &item : a)
    if (!excluded.count(item) && seen.insert(item).second) out.push_back(item);
  return out;
}

std::vector<std::string> set_union(const std::vector<std::string> &a,
                                   const std::vector<std::string> &b) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> out;
  for (const auto *list : {&a, &b})
    for (const auto &item : *list)
      if (seen.insert(item).second) out.push_back(item);
  return out;
}

static int edit_distance(const std::string &a, const std::string &b) {
  const size_t n = a.size(), m = b.size();
  std::vector<int> prev(m + 1), curr(m + 1), before(m + 1);
  for (size_t j = 0; j <= m; j++) prev[j] = static_cast<int>(j);
  for (size_t i = 1; i <= n; i++) {
    curr[0] = static_cast<int>(i);
    for (size_t j = 1; j <= m; j++) {
      int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
        curr[j] = std::min(curr[j], before[j - 2] + cost);
    }
    before = prev;
    prev = curr;
  }
  return prev[m];
}

std::vector<std::string> suggest(const std::string &name,
                                 const std::vector<std::string> &candidates) {
  const int threshold = std::max<int64_t>(1, static_cast<int64_t>(name.size()) / 4);
  std::vector<std::pair<int, std::string>> matches;
  for (const auto &candidate : candidates) {
    int d = edit_distance(name, candidate);
    if (d <= threshold) matches.emplace_back(d, candidate);
  }
  std::sort(matches.begin(), matches.end());
  std::vector<std::string> out;
  for (auto &m : matches) out.push_back(std::move(m.second));
  return out;
}

}
