#include "strlib.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace lmhtml::strlib {

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
