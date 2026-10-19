#include "analyze.hpp"
#include "strlib.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

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

// HTML boolean attributes: presence is the value.
const std::set<std::string> &boolean_attrs() {
  static const std::set<std::string> names = {
      "allowfullscreen", "async", "autofocus", "autoplay", "checked", "controls", "default",
      "defer", "disabled", "formnovalidate", "hidden", "inert", "ismap", "itemscope", "loop",
      "multiple", "muted", "nomodule", "novalidate", "open", "playsinline", "readonly",
      "required", "reversed", "selected"};
  return names;
}

// Sorted attribute map with class tokens sorted, data-* dropped and boolean
// attribute values ignored.
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
    if (boolean_attrs().count(n)) v.clear(); // disabled == disabled="disabled"
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

// Whitespace is content inside these (and their descendants).
bool preserves_space(lxb_dom_node_t *node) {
  std::string tag = tag_name(node);
  return tag == "pre" || tag == "textarea" || tag == "listing" || tag == "plaintext";
}
bool inside_preformatted(lxb_dom_node_t *node) {
  for (lxb_dom_node_t *n = node; n; n = n->parent)
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && preserves_space(n)) return true;
  return false;
}

std::string node_text(lxb_dom_node_t *c) {
  size_t len = 0;
  const lxb_char_t *d = lxb_dom_node_text_content(c, &len);
  return d ? std::string(reinterpret_cast<const char *>(d), len) : "";
}

// The node's own text, positioned: each non-empty text run is prefixed with
// the number of element children before it, so moving text across a child
// element ("a<b>x</b>b" vs "ab<b>x</b>") is a change.
std::string text_signature(lxb_dom_node_t *node, bool preserve) {
  std::string out;
  size_t elements = 0;
  for (lxb_dom_node_t *c = node->first_child; c; c = c->next) {
    if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) elements++;
    else if (c->type == LXB_DOM_NODE_TYPE_TEXT || c->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
      std::string seg = preserve ? node_text(c) : normalize_text(node_text(c));
      if (!seg.empty()) out += std::to_string(elements) + ":" + seg + '\x1f';
    }
  }
  return out;
}

using HashCache = std::unordered_map<lxb_dom_node_t *, uint64_t>;

uint64_t hash_node(lxb_dom_node_t *node, bool preserve, HashCache *cache) {
  if (cache) {
    auto it = cache->find(node);
    if (it != cache->end()) return it->second;
  }
  Hash h;
  h.str(tag_name(node));
  for (const auto &[name, value] : norm_attrs(node)) { h.str(name); h.str(value); }
  h.str(text_signature(node, preserve));
  auto kids = element_children(node);
  h.num(kids.size());
  for (auto *c : kids) h.num(hash_node(c, preserve || preserves_space(c), cache));
  if (cache) (*cache)[node] = h.h;
  return h.h;
}

uint64_t hash_node(lxb_dom_node_t *node) {
  return hash_node(node, inside_preformatted(node), nullptr);
}

// Exact normalized equality: the hash is only a filter, a 64-bit FNV match is
// confirmed before two subtrees are treated as identical.
bool same_tree(lxb_dom_node_t *a, lxb_dom_node_t *b, bool preserve) {
  if (tag_name(a) != tag_name(b) || norm_attrs(a) != norm_attrs(b) ||
      text_signature(a, preserve) != text_signature(b, preserve))
    return false;
  auto ca = element_children(a), cb = element_children(b);
  if (ca.size() != cb.size()) return false;
  for (size_t i = 0; i < ca.size(); i++)
    if (!same_tree(ca[i], cb[i], preserve || preserves_space(ca[i]))) return false;
  return true;
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

// Full path from the html root, e.g. html > body:nth-child(2) > nav.site-nav:nth-child(2).
std::string path_of(lxb_dom_node_t *node) {
  std::vector<std::string> parts;
  for (lxb_dom_node_t *n = node; n; n = parent_node(n)) {
    parts.push_back(tag_name(n) == "html" ? "html" : segment(n));
    if (tag_name(n) == "html") break;
  }
  std::string out;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!out.empty()) out += " > ";
    out += *it;
  }
  return out;
}

// Sibling lists whose LCS table (after trimming the common prefix/suffix)
// would exceed this many cells use greedy in-order matching instead; a large
// reorder may then show extra added/removed pairs, never unbounded memory.
const size_t LCS_MAX_CELLS = size_t(4) << 20;

struct DiffContext {
  const std::string &file_a, &file_b;
  std::vector<DiffFinding> &out;
  HashCache cache_a, cache_b;
};

void diff_elements(lxb_dom_node_t *a, lxb_dom_node_t *b, const std::string &path_a,
                   const std::string &path_b, bool preserve, DiffContext &cx) {
  auto attrs_a = norm_attrs(a), attrs_b = norm_attrs(b);
  if (attrs_a != attrs_b) {
    std::set<std::string> names;
    for (const auto &[n, v] : attrs_a) if (!attrs_b.count(n) || attrs_b.at(n) != v) names.insert(n);
    for (const auto &[n, v] : attrs_b) if (!attrs_a.count(n)) names.insert(n);
    std::string list;
    for (const auto &n : names) { if (!list.empty()) list += ","; list += n; }
    cx.out.push_back({"diff-changed", cx.file_a, path_a + ": attributes differ: " + list, 0});
  }
  if (text_signature(a, preserve) != text_signature(b, preserve))
    cx.out.push_back({"diff-changed", cx.file_a, path_a + ": text differs", 0});

  auto ca = element_children(a), cb = element_children(b);
  auto keep = [&](lxb_dom_node_t *c) { return preserve || preserves_space(c); };
  std::vector<uint64_t> ha, hb;
  for (auto *c : ca) ha.push_back(hash_node(c, keep(c), &cx.cache_a));
  for (auto *c : cb) hb.push_back(hash_node(c, keep(c), &cx.cache_b));
  const size_t n = ha.size(), m = hb.size();
  std::vector<bool> used_a(n, false), used_b(m, false);
  auto identical = [&](size_t i, size_t j) {
    return ha[i] == hb[j] && same_tree(ca[i], cb[j], keep(ca[i]));
  };
  // Common prefix and suffix first (the usual case: a local edit).
  size_t lo = 0;
  while (lo < n && lo < m && identical(lo, lo)) { used_a[lo] = used_b[lo] = true; lo++; }
  size_t hi_a = n, hi_b = m;
  while (hi_a > lo && hi_b > lo && identical(hi_a - 1, hi_b - 1)) {
    used_a[--hi_a] = true;
    used_b[--hi_b] = true;
  }
  const size_t rn = hi_a - lo, rm = hi_b - lo;
  if (rn && rm && (rn + 1) * (rm + 1) <= LCS_MAX_CELLS) {
    // LCS over child hashes in the middle; matches are verified.
    std::vector<uint32_t> dp((rn + 1) * (rm + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint32_t & { return dp[i * (rm + 1) + j]; };
    for (size_t i = rn; i-- > 0;)
      for (size_t j = rm; j-- > 0;)
        at(i, j) = ha[lo + i] == hb[lo + j] ? at(i + 1, j + 1) + 1
                                            : std::max(at(i + 1, j), at(i, j + 1));
    for (size_t i = 0, j = 0; i < rn && j < rm;) {
      if (ha[lo + i] == hb[lo + j]) {
        if (same_tree(ca[lo + i], cb[lo + j], keep(ca[lo + i])))
          used_a[lo + i] = used_b[lo + j] = true;
        i++; j++;
      } else if (at(i + 1, j) >= at(i, j + 1)) i++;
      else j++;
    }
  } else if (rn && rm) {
    // Greedy in-order matching: each left child takes the next identical
    // right child after the previous match.
    std::unordered_map<uint64_t, std::vector<size_t>> positions;
    for (size_t j = lo; j < hi_b; j++) positions[hb[j]].push_back(j);
    size_t next_j = lo;
    for (size_t i = lo; i < hi_a; i++) {
      auto it = positions.find(ha[i]);
      if (it == positions.end()) continue;
      auto &v = it->second;
      auto pos = std::lower_bound(v.begin(), v.end(), next_j);
      if (pos != v.end() && same_tree(ca[i], cb[*pos], keep(ca[i]))) {
        used_a[i] = used_b[*pos] = true;
        next_j = *pos + 1;
      }
    }
  }
  // Pair remaining children in order when tags agree and recurse; identical
  // leftovers are moves and show as removed + added.
  std::map<std::string, std::vector<size_t>> rest_b;
  for (size_t j = 0; j < m; j++) if (!used_b[j]) rest_b[tag_name(cb[j])].push_back(j);
  std::map<std::string, size_t> taken;
  std::vector<bool> done_b(m, false);
  for (size_t i = 0; i < n; i++) {
    if (used_a[i]) continue;
    std::string tag = tag_name(ca[i]);
    std::string seg_a = path_a + " > " + segment(ca[i]);
    auto it = rest_b.find(tag);
    size_t &next = taken[tag];
    if (it == rest_b.end() || next >= it->second.size() || identical(i, it->second[next])) {
      cx.out.push_back({"diff-removed", cx.file_a, seg_a, 0});
      continue;
    }
    size_t j = it->second[next++];
    done_b[j] = true;
    diff_elements(ca[i], cb[j], seg_a, path_b + " > " + segment(cb[j]), keep(ca[i]), cx);
  }
  for (size_t j = 0; j < m; j++)
    if (!used_b[j] && !done_b[j])
      cx.out.push_back({"diff-added", cx.file_b, path_b + " > " + segment(cb[j]), 0});
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
  DiffContext cx{a.path, b.path, out, {}, {}};
  diff_elements(root_a, root_b, "html", "html", false, cx);
  return out.size() - before;
}

namespace {

bool substantial(lxb_dom_node_t *node) {
  size_t elements = 0;
  std::function<void(lxb_dom_node_t *)> count = [&](lxb_dom_node_t *n) {
    for (lxb_dom_node_t *c = n->first_child; c; c = c->next)
      if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) { elements++; count(c); }
  };
  count(node);
  if (elements >= 2) return true;
  return normalize_text(text_content(node)).size() >= 40;
}

void hash_all(lxb_dom_node_t *node, std::map<lxb_dom_node_t *, uint64_t> &hashes,
              std::set<uint64_t> &page_hashes) {
  if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
    uint64_t h = hash_node(node);
    hashes[node] = h;
    page_hashes.insert(h);
  }
  for (auto *c = node->first_child; c; c = c->next) hash_all(c, hashes, page_hashes);
}

std::string open_tag(lxb_dom_node_t *node) {
  std::string out = "<" + tag_name(node);
  lxb_dom_element_t *el = lxb_dom_interface_element(node);
  for (lxb_dom_attr_t *a = el->first_attr; a; a = a->next) {
    size_t nlen = 0, vlen = 0;
    const lxb_char_t *name = lxb_dom_attr_local_name(a, &nlen);
    const lxb_char_t *value = lxb_dom_attr_value(a, &vlen);
    std::string v = value ? std::string(reinterpret_cast<const char *>(value), vlen) : "";
    std::string esc;
    for (char c : v) {
      if (c == '&') esc += "&amp;";
      else if (c == '"') esc += "&quot;";
      else if (c == '<') esc += "&lt;";
      else esc += c;
    }
    out += " " + std::string(reinterpret_cast<const char *>(name), nlen) + "=\"" + esc + "\"";
  }
  return out + ">";
}

bool is_void_tag(const std::string &tag) {
  static const std::set<std::string> void_tags = {
      "area", "base", "br", "col", "embed", "hr", "img", "input",
      "link", "meta", "param", "source", "track", "wbr"};
  return void_tags.count(tag) != 0;
}

} // namespace

TemplateResult make_template(std::vector<Document *> &pages, int64_t k,
                             const std::string &tpl_path) {
  if (pages.empty()) throw std::string("Html.write_template: empty document collection");
  if (k < 1) throw std::string("Html.write_template: k must be at least 1");
  if (static_cast<uint64_t>(k) > pages.size())
    throw std::string("Html.write_template: k (" + std::to_string(k) + ") exceeds the " +
                      std::to_string(pages.size()) + " pages; nothing could be common");

  // Hash every subtree of every page once; coverage = pages containing a hash.
  std::vector<std::map<lxb_dom_node_t *, uint64_t>> per_page(pages.size());
  std::map<uint64_t, std::set<size_t>> coverage;
  std::map<uint64_t, lxb_dom_node_t *> representative; // first subtree seen per hash
  for (size_t i = 0; i < pages.size(); i++) {
    std::set<uint64_t> page_hashes;
    hash_all(lxb_dom_interface_node(pages[i]->tree), per_page[i], page_hashes);
    for (const auto &[node, h] : per_page[i]) {
      auto [rep, fresh] = representative.emplace(h, node);
      // A 64-bit FNV match counts only when the subtrees really are equal.
      if (fresh || coverage[h].count(i) ||
          same_tree(node, rep->second, inside_preformatted(node)))
        coverage[h].insert(i);
    }
  }
  // skeleton: page richest in substantial common subtrees (ties: first)
  std::map<lxb_dom_node_t *, uint64_t> *skeleton_hashes = nullptr;
  size_t skeleton = 0, best = 0;
  std::vector<size_t> common_count(pages.size(), 0);
  for (size_t i = 0; i < pages.size(); i++)
    for (const auto &[node, h] : per_page[i])
      if (coverage[h].size() >= static_cast<size_t>(k) && substantial(node))
        common_count[i]++;
  for (size_t i = 0; i < pages.size(); i++)
    if (common_count[i] > best) { best = common_count[i]; skeleton = i; }
  skeleton_hashes = &per_page[skeleton];

  auto is_common = [&](lxb_dom_node_t *node) {
    auto it = skeleton_hashes->find(node);
    return it != skeleton_hashes->end() &&
           coverage[it->second].size() >= static_cast<size_t>(k) && substantial(node);
  };

  TemplateResult result;
  size_t slot_counter = 0;
  std::function<void(lxb_dom_node_t *)> emit_children = [&](lxb_dom_node_t *node) {
    for (lxb_dom_node_t *c = node->first_child; c; c = c->next) {
      if (c->type == LXB_DOM_NODE_TYPE_TEXT && !normalize_text(text_content(c)).empty()) {
        // text directly inside a varying element is page content: a slot,
        // never the skeleton page's own words
        result.markup += "<!-- slot:text-" + std::to_string(++slot_counter) + " -->";
        continue;
      }
      if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
      if (is_common(c)) {
        result.findings.push_back(
            {"common-part", tpl_path,
             path_of(c) + " shared by " +
                 std::to_string(coverage[(*skeleton_hashes)[c]].size()) + " of " +
                 std::to_string(pages.size()) + " pages",
             0});
        result.markup += outer_html(c);
        continue;
      }
      // recurse when a varying subtree still contains shared parts deeper down
      bool has_common_descendant = false;
      std::function<bool(lxb_dom_node_t *)> scan = [&](lxb_dom_node_t *n) {
        for (auto *cc : element_children(n))
          if (is_common(cc) || scan(cc)) return true;
        return false;
      };
      has_common_descendant = scan(c);
      if (has_common_descendant) {
        result.markup += open_tag(c);
        emit_children(c);
        if (!is_void_tag(tag_name(c))) result.markup += "</" + tag_name(c) + ">";
      } else {
        result.markup += "<!-- slot:" + tag_name(c) + "-" + std::to_string(++slot_counter) + " -->";
      }
    }
  };

  lxb_dom_node_t *root = nullptr;
  for (auto *c : element_children(lxb_dom_interface_node(pages[skeleton]->tree)))
    if (tag_name(c) == "html") root = c;
  if (!root) throw std::string("Html.write_template: skeleton page has no html element");
  // Keep the skeleton's doctype: without it the template renders in quirks mode.
  for (lxb_dom_node_t *c = lxb_dom_interface_node(pages[skeleton]->tree)->first_child; c;
       c = c->next)
    if (c->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) result.markup += outer_html(c) + "\n";
  if (is_common(root)) {
    result.markup += outer_html(root) + "\n";
    result.findings.push_back({"common-part", tpl_path,
                               "html shared by " +
                                   std::to_string(coverage[(*skeleton_hashes)[root]].size()) +
                                   " of " + std::to_string(pages.size()) + " pages",
                               0});
  } else {
    result.markup += open_tag(root);
    emit_children(root);
    result.markup += "</html>\n";
  }
  result.slots = slot_counter;
  return result;
}

}
