#include "analyze.hpp"
#include "strlib.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace lmhtml {
namespace {

const uint64_t FNV_OFFSET = 14695981039346656037ull;
const uint64_t FNV_PRIME = 1099511628211ull;

struct Hash {
  uint64_t h = FNV_OFFSET;
  void bytes(const void *p, size_t n) {
    const unsigned char *c = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= FNV_PRIME; }
  }
  void str(const std::string &s) { // length-prefixed: no field-boundary ambiguity
    uint64_t n = s.size();
    bytes(&n, sizeof n);
    bytes(s.data(), s.size());
  }
  void num(uint64_t v) { bytes(&v, sizeof v); }
};

std::string normalize_text(const std::string &s) {
  std::string out;
  bool pending_space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) { pending_space = !out.empty(); continue; }
    if (pending_space) { out += ' '; pending_space = false; }
    out += static_cast<char>(c);
  }
  return out;
}

// Sorted attribute map with class tokens sorted and data-* dropped.
std::map<std::string, std::string> norm_attrs(lxb_dom_node_t *node) {
  std::map<std::string, std::string> out;
  lxb_dom_element_t *el = lxb_dom_interface_element(node);
  if (!el || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return out;
  lxb_dom_attr_t *attr = el->first_attr;
  for (; attr; attr = attr->next) {
    size_t nlen = 0, vlen = 0;
    const lxb_char_t *name = lxb_dom_attr_local_name(attr, &nlen);
    const lxb_char_t *value = lxb_dom_attr_value(attr, &vlen);
    std::string n(reinterpret_cast<const char *>(name), nlen);
    std::string v = value ? std::string(reinterpret_cast<const char *>(value), vlen) : "";
    if (strlib::starts_with(n, "data-")) continue;
    if (n == "class") {
      std::vector<std::string> tokens;
      std::string cur;
      for (char c : v) {
        if (std::isspace(static_cast<unsigned char>(c))) {
          if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else cur += c;
      }
      if (!cur.empty()) tokens.push_back(cur);
      std::sort(tokens.begin(), tokens.end());
      v.clear();
      for (size_t i = 0; i < tokens.size(); i++) { if (i) v += ' '; v += tokens[i]; }
    }
    out[n] = v;
  }
  return out;
}

std::vector<lxb_dom_node_t *> element_children(lxb_dom_node_t *node) {
  std::vector<lxb_dom_node_t *> out;
  for (lxb_dom_node_t *c = node->first_child; c; c = c->next)
    if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) out.push_back(c);
  return out;
}

std::string direct_text(lxb_dom_node_t *node) {
  std::string raw;
  for (lxb_dom_node_t *c = node->first_child; c; c = c->next)
    if (c->type == LXB_DOM_NODE_TYPE_TEXT || c->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
      size_t len = 0;
      const lxb_char_t *d = lxb_dom_node_text_content(c, &len);
      if (d) raw.append(reinterpret_cast<const char *>(d), len);
    }
  return normalize_text(raw);
}

uint64_t hash_node(lxb_dom_node_t *node) {
  Hash h;
  h.str(tag_name(node));
  for (const auto &[name, value] : norm_attrs(node)) { h.str(name); h.str(value); }
  h.str(direct_text(node));
  auto kids = element_children(node);
  h.num(kids.size());
  for (auto *c : kids) h.num(hash_node(c));
  return h.h;
}

std::string segment(lxb_dom_node_t *node) {
  std::string seg = tag_name(node);
  std::string cls = attr(node, "class");
  if (!cls.empty()) {
    size_t end = cls.find_first_of(" \t\n");
    seg += "." + cls.substr(0, end);
  }
  size_t index = 1;
  for (lxb_dom_node_t *s = node->prev; s; s = s->prev)
    if (s->type == LXB_DOM_NODE_TYPE_ELEMENT) index++;
  seg += ":nth-child(" + std::to_string(index) + ")";
  return seg;
}

void diff_elements(lxb_dom_node_t *a, lxb_dom_node_t *b, const std::string &path,
                   const std::string &file_a, const std::string &file_b,
                   std::vector<DiffFinding> &out) {
  auto attrs_a = norm_attrs(a), attrs_b = norm_attrs(b);
  if (attrs_a != attrs_b) {
    std::set<std::string> names;
    for (const auto &[n, v] : attrs_a) if (!attrs_b.count(n) || attrs_b.at(n) != v) names.insert(n);
    for (const auto &[n, v] : attrs_b) if (!attrs_a.count(n)) names.insert(n);
    std::string list;
    for (const auto &n : names) { if (!list.empty()) list += ","; list += n; }
    out.push_back({"diff-changed", file_a, path + ": attributes differ: " + list, 0});
  }
  std::string text_a = direct_text(a), text_b = direct_text(b);
  if (text_a != text_b)
    out.push_back({"diff-changed", file_a, path + ": text differs", 0});

  auto ca = element_children(a), cb = element_children(b);
  std::vector<uint64_t> ha, hb;
  for (auto *c : ca) ha.push_back(hash_node(c));
  for (auto *c : cb) hb.push_back(hash_node(c));

  // LCS over child hashes: matched pairs are identical subtrees.
  const size_t n = ha.size(), m = hb.size();
  std::vector<std::vector<size_t>> dp(n + 1, std::vector<size_t>(m + 1, 0));
  for (size_t i = n; i-- > 0;)
    for (size_t j = m; j-- > 0;)
      dp[i][j] = ha[i] == hb[j] ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);
  std::vector<bool> used_a(n, false), used_b(m, false);
  for (size_t i = 0, j = 0; i < n && j < m;) {
    if (ha[i] == hb[j]) { used_a[i] = used_b[j] = true; i++; j++; }
    else if (dp[i + 1][j] >= dp[i][j + 1]) i++;
    else j++;
  }
  // Pair remaining unmatched children in order when tags agree; recurse.
  std::vector<size_t> rest_a, rest_b;
  for (size_t i = 0; i < n; i++) if (!used_a[i]) rest_a.push_back(i);
  for (size_t j = 0; j < m; j++) if (!used_b[j]) rest_b.push_back(j);
  size_t paired = 0;
  std::vector<bool> done_b(rest_b.size(), false);
  for (size_t i = 0; i < rest_a.size(); i++) {
    lxb_dom_node_t *na = ca[rest_a[i]];
    size_t match = rest_b.size();
    for (size_t j = 0; j < rest_b.size(); j++)
      if (!done_b[j] && tag_name(cb[rest_b[j]]) == tag_name(na)) { match = j; break; }
    if (match < rest_b.size()) {
      done_b[match] = true;
      paired++;
      diff_elements(na, cb[rest_b[match]], path + " > " + segment(na), file_a, file_b, out);
    } else {
      out.push_back({"diff-removed", file_a, path + " > " + segment(na), 0});
    }
  }
  for (size_t j = 0; j < rest_b.size(); j++)
    if (!done_b[j])
      out.push_back({"diff-added", file_b, path + " > " + segment(cb[rest_b[j]]), 0});
  (void) paired;
}

} // namespace

uint64_t subtree_hash(lxb_dom_node_t *node) { return hash_node(node); }

size_t diff_documents(Document &a, Document &b, std::vector<DiffFinding> &out) {
  lxb_dom_node_t *root_a = nullptr, *root_b = nullptr;
  for (auto *c : element_children(lxb_dom_interface_node(a.tree)))
    if (tag_name(c) == "html") root_a = c;
  for (auto *c : element_children(lxb_dom_interface_node(b.tree)))
    if (tag_name(c) == "html") root_b = c;
  if (!root_a || !root_b) throw std::string("document has no html element");
  size_t before = out.size();
  diff_elements(root_a, root_b, "html", a.path, b.path, out);
  return out.size() - before;
}

}
