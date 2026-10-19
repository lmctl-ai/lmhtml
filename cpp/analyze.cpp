#include "analyze.hpp"
#include "strlib.hpp"
#include <algorithm>
#include <functional>
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

  // Hash every subtree of every page once; coverage = pages containing a hash.
  std::vector<std::map<lxb_dom_node_t *, uint64_t>> per_page(pages.size());
  std::map<uint64_t, std::set<size_t>> coverage;
  for (size_t i = 0; i < pages.size(); i++) {
    std::set<uint64_t> page_hashes;
    hash_all(lxb_dom_interface_node(pages[i]->tree), per_page[i], page_hashes);
    for (uint64_t h : page_hashes) coverage[h].insert(i);
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
    for (auto *c : element_children(node)) {
      if (is_common(c)) {
        result.findings.push_back(
            {"common-part", tpl_path,
             tag_name(c) + " shared by " +
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
  if (is_common(root)) {
    result.markup = outer_html(root);
    result.findings.push_back({"common-part", tpl_path,
                               "html shared by " + std::to_string(pages.size()) + " of " +
                                   std::to_string(pages.size()) + " pages",
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
