#include "strlib.hpp"
#include <algorithm>
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

bool match(const std::string &text, const std::string &pattern) {
  size_t t = 0, p = 0, star_p = std::string::npos, star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size() &&
        (pattern[p] == '?' || pattern[p] == text[t])) {
      t++; p++;
    } else if (p < pattern.size() && pattern[p] == '[') {
      size_t q = p + 1;
      bool negate = q < pattern.size() && pattern[q] == '!';
      if (negate) q++;
      bool hit = false;
      char first = 0;
      while (q < pattern.size() && (pattern[q] != ']' || q == p + 1 + (negate ? 1 : 0))) {
        if (q + 2 < pattern.size() && pattern[q + 1] == '-' && pattern[q + 2] != ']') {
          if (text[t] >= pattern[q] && text[t] <= pattern[q + 2]) hit = true;
          q += 3;
        } else {
          if (pattern[q] == text[t]) hit = true;
          first = pattern[q];
          q++;
        }
      }
      (void) first;
      if (q >= pattern.size()) return false; // unterminated class
      if (hit == negate) { goto star_backtrack; }
      t++; p = q + 1;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++; star_t = t;
    } else {
    star_backtrack:
      if (star_p == std::string::npos) return false;
      p = star_p + 1;
      t = ++star_t;
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
