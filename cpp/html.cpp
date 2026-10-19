#include "html.hpp"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <lexbor/css/css.h>
#include <lexbor/css/rule.h>
#include <lexbor/css/stylesheet.h>
#include <lexbor/selectors/selectors.h>

namespace lmhtml {

Document::~Document() {
  if (tree) lxb_html_document_destroy(tree);
}

std::unique_ptr<Document> parse_html(const std::string &html, const std::string &path) {
  check_depth_prescan(html, path);
  auto doc = std::make_unique<Document>();
  doc->tree = lxb_html_document_create();
  if (!doc->tree) throw std::string("out of memory creating document");
  doc->path = path;
  lxb_status_t status = lxb_html_document_parse(
      doc->tree, reinterpret_cast<const lxb_char_t *>(html.data()), html.size());
  if (status != LXB_STATUS_OK) throw std::string("HTML parse failed for ") + path;
  check_depth_dom(*doc); // authoritative check on the real tree
  return doc;
}

// Cheap byte-level estimate before parsing: pathological nesting (hundreds of
// thousands of open tags) stalls the parser, so refuse early. It only guards
// the parse time, so it allows PRESCAN_NESTING (4x the real limit) and tracks
// a tag stack: close tags pop to their opener, and tags whose start implies
// the end of an open sibling (<li>, <td>, <p>, ...) pop it, so unclosed list
// items or cells are not mistaken for nesting. The DOM check below enforces
// MAX_NESTING exactly.
void check_depth_prescan(const std::string &html, const std::string &path) {
  static const std::set<std::string> void_tags = {
      "area", "base", "br", "col", "embed", "hr", "img", "input",
      "link", "meta", "param", "source", "track", "wbr"};
  // Content up to the matching end tag is text, not markup.
  static const std::set<std::string> raw_text = {"script", "style", "textarea", "title",
                                                 "xmp", "iframe", "noembed", "noframes"};
  // Opening one of these closes a still-open element of the same group.
  static const std::map<std::string, std::string> implied_group = {
      {"li", "li"}, {"p", "p"}, {"dt", "d"}, {"dd", "d"}, {"option", "option"},
      {"optgroup", "optgroup"}, {"tr", "tr"}, {"td", "cell"}, {"th", "cell"},
      {"thead", "tsection"}, {"tbody", "tsection"}, {"tfoot", "tsection"},
      {"rb", "ruby"}, {"rt", "ruby"}, {"rp", "ruby"}, {"rtc", "ruby"}};
  // ...but only within its own container: a nested list/table/select starts
  // a new scope (the parser nests <ul><li><ul><li> for real).
  static const std::map<std::string, std::set<std::string>> group_scope = {
      {"li", {"ul", "ol", "menu"}}, {"d", {"dl"}}, {"option", {"select", "datalist"}},
      {"optgroup", {"select", "datalist"}}, {"tr", {"table"}}, {"cell", {"table", "tr"}},
      {"tsection", {"table"}}, {"ruby", {"ruby"}}, {"p", {"button"}}};
  static const std::set<std::string> any_scope = {"html", "body", "td", "th", "caption",
                                                  "template", "object", "marquee", "applet"};
  const size_t PRESCAN_NESTING = 4 * MAX_NESTING;
  auto group_of = [](const std::string &tag) {
    auto it = implied_group.find(tag);
    return it == implied_group.end() ? std::string() : it->second;
  };
  const size_t n = html.size();
  size_t i = 0;
  std::vector<std::string> open;
  std::string where = path.empty() ? "<string>" : path;
  while (i < n) {
    if (html[i] != '<') { i++; continue; }
    if (html.compare(i, 4, "<!--") == 0) {
      // "<!-->" and "<!--->" are complete (empty) comments; "--!>" also ends one.
      if (html.compare(i + 4, 1, ">") == 0) { i += 5; continue; }
      if (html.compare(i + 4, 2, "->") == 0) { i += 6; continue; }
      size_t end = html.find("--", i + 4);
      while (end != std::string::npos && html.compare(end + 2, 1, ">") != 0 &&
             html.compare(end + 2, 2, "!>") != 0)
        end = html.find("--", end + 1);
      if (end == std::string::npos) break;
      i = end + (html[end + 2] == '>' ? 3 : 4);
      continue;
    }
    if (i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
      size_t end = html.find('>', i + 2);
      i = end == std::string::npos ? n : end + 1;
      continue;
    }
    bool closing = i + 1 < n && html[i + 1] == '/';
    size_t j = i + 1 + (closing ? 1 : 0);
    if (j >= n || !std::isalpha(static_cast<unsigned char>(html[j]))) { i++; continue; } // text "<"
    size_t name_start = j;
    while (j < n && !std::isspace(static_cast<unsigned char>(html[j])) && html[j] != '/' &&
           html[j] != '>')
      j++;
    std::string name = html.substr(name_start, j - name_start);
    for (auto &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Scan to '>'; quotes only delimit a value right after '=' (a stray quote
    // elsewhere is part of a name, as in the tokenizer).
    char quote = 0;
    bool self_closing = false, after_equals = false;
    while (j < n) {
      char c = html[j];
      if (quote) { if (c == quote) quote = 0; }
      else if (after_equals && (c == '"' || c == '\'')) { quote = c; after_equals = false; }
      else if (c == '>') break;
      else if (c == '=') after_equals = true;
      else if (!std::isspace(static_cast<unsigned char>(c))) {
        after_equals = false;
        if (c == '/' && j + 1 < n && html[j + 1] == '>') self_closing = true;
      }
      j++;
    }
    if (j >= n) break; // unterminated tag: the parser drops it
    j++;
    if (closing) {
      for (size_t k = open.size(); k-- > 0;)
        if (open[k] == name) { open.resize(k); break; }
      i = j;
      continue;
    }
    if (name == "plaintext") break; // everything after is text
    if (raw_text.count(name)) {
      // skip to the matching close tag, ASCII case-insensitively
      size_t end = j;
      for (; end < n; end++) {
        if (html[end] != '<' || end + 1 >= n || html[end + 1] != '/') continue;
        if (end + 2 + name.size() > n) { end = n; break; }
        bool match = true;
        for (size_t k = 0; k < name.size() && match; k++)
          match = std::tolower(static_cast<unsigned char>(html[end + 2 + k])) == name[k];
        if (match) break;
      }
      i = end;
      continue;
    }
    std::string group = group_of(name);
    if (!group.empty()) {
      const auto &scope = group_scope.at(group);
      for (size_t k = open.size(); k-- > 0 && open.size() - k <= 8;) {
        if (group_of(open[k]) == group) { open.resize(k); break; }
        if (scope.count(open[k]) || (any_scope.count(open[k]) && group != "cell")) break;
      }
    }
    if (!self_closing && !void_tags.count(name)) {
      open.push_back(name);
      if (open.size() > PRESCAN_NESTING)
        throw std::string("nesting depth exceeds " + std::to_string(MAX_NESTING) + " in " +
                          where);
    }
    i = j;
  }
}

void check_depth_dom(Document &doc) {
  std::vector<std::pair<lxb_dom_node_t *, size_t>> stack;
  stack.push_back({lxb_dom_interface_node(doc.tree), 0});
  while (!stack.empty()) {
    auto [node, depth] = stack.back();
    stack.pop_back();
    if (depth > MAX_NESTING)
      throw std::string("nesting depth exceeds " + std::to_string(MAX_NESTING) + " in " +
                        (doc.path.empty() ? "<string>" : doc.path));
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next)
      stack.push_back({child, depth + 1});
  }
}

static lxb_status_t collect_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t, void *ctx) {
  static_cast<std::vector<lxb_dom_node_t *> *>(ctx)->push_back(node);
  return LXB_STATUS_OK;
}

std::vector<lxb_dom_node_t *> select(lxb_dom_node_t *scope, const std::string &css) {
  lxb_css_parser_t *parser = lxb_css_parser_create();
  if (!parser || lxb_css_parser_init(parser, nullptr) != LXB_STATUS_OK)
    throw std::string("out of memory creating CSS parser");
  lxb_css_selector_list_t *list = lxb_css_selectors_parse(
      parser, reinterpret_cast<const lxb_char_t *>(css.data()), css.size());
  if (!list) {
    lxb_css_parser_destroy(parser, true);
    throw std::string("invalid selector: ") + css;
  }
  lxb_selectors_t *selectors = lxb_selectors_create();
  if (!selectors || lxb_selectors_init(selectors) != LXB_STATUS_OK) {
    lxb_css_parser_destroy(parser, true);
    lxb_css_selector_list_destroy_memory(list);
    throw std::string("out of memory creating selectors");
  }
  std::vector<lxb_dom_node_t *> matches;
  lxb_selectors_find(selectors, scope, list, collect_cb, &matches);
  lxb_selectors_destroy(selectors, true);
  lxb_css_parser_destroy(parser, true);
  lxb_css_selector_list_destroy_memory(list); // the list has its own memory pool
  return matches;
}

static lxb_dom_element_t *as_element(lxb_dom_node_t *node) {
  return node && node->type == LXB_DOM_NODE_TYPE_ELEMENT
             ? lxb_dom_interface_element(node)
             : nullptr;
}

std::string tag_name(lxb_dom_node_t *node) {
  size_t len = 0;
  // HTML element tag names come back uppercase from the DOM; normalize.
  const lxb_char_t *name = lxb_dom_node_name(node, &len);
  std::string out(reinterpret_cast<const char *>(name), len);
  for (auto &c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::string attr(lxb_dom_node_t *node, const std::string &name) {
  lxb_dom_element_t *el = as_element(node);
  if (!el) return "";
  size_t len = 0;
  const lxb_char_t *value = lxb_dom_element_get_attribute(
      el, reinterpret_cast<const lxb_char_t *>(name.data()), name.size(), &len);
  return value ? std::string(reinterpret_cast<const char *>(value), len) : "";
}

bool has_attr(lxb_dom_node_t *node, const std::string &name) {
  lxb_dom_element_t *el = as_element(node);
  return el && lxb_dom_element_has_attribute(
                   el, reinterpret_cast<const lxb_char_t *>(name.data()), name.size());
}

std::string text_content(lxb_dom_node_t *node) {
  size_t len = 0;
  // Buffer lives in the document's own memory; freed with the document.
  const lxb_char_t *text = lxb_dom_node_text_content(node, &len);
  return text ? std::string(reinterpret_cast<const char *>(text), len) : "";
}

static lxb_status_t append_cb(const lxb_char_t *data, size_t len, void *ctx) {
  static_cast<std::string *>(ctx)->append(reinterpret_cast<const char *>(data), len);
  return LXB_STATUS_OK;
}

std::string outer_html(lxb_dom_node_t *node) {
  std::string out;
  if (lxb_html_serialize_tree_cb(node, append_cb, &out) != LXB_STATUS_OK)
    throw std::string("serialization failed");
  return out;
}

lxb_dom_node_t *parent_node(lxb_dom_node_t *node) {
  lxb_dom_node_t *parent = node->parent;
  while (parent && parent->type != LXB_DOM_NODE_TYPE_ELEMENT) parent = parent->parent;
  return parent;
}

static void walk(lxb_dom_node_t *node, const std::function<void(lxb_dom_node_t *)> &fn) {
  for (lxb_dom_node_t *child = node->first_child; child; child = child->next) {
    fn(child);
    walk(child, fn);
  }
}

std::vector<std::string> collect_links(Document &doc) {
  std::vector<std::string> links;
  walk(lxb_dom_interface_node(doc.tree), [&](lxb_dom_node_t *node) {
    lxb_dom_element_t *el = as_element(node);
    if (!el) return;
    std::string tag = tag_name(node);
    std::string name;
    if (tag == "a" || tag == "link") name = "href";
    else if (tag == "img" || tag == "script") name = "src";
    else return;
    std::string value = attr(node, name);
    if (!value.empty()) links.push_back(value);
  });
  return links;
}

bool has_anchor(Document &doc, const std::string &fragment) {
  bool found = false;
  walk(lxb_dom_interface_node(doc.tree), [&](lxb_dom_node_t *node) {
    if (found) return;
    if (has_attr(node, "id") && attr(node, "id") == fragment) found = true;
    else if (tag_name(node) == "a" && has_attr(node, "name") && attr(node, "name") == fragment)
      found = true;
  });
  return found;
}

std::vector<std::string> classes_used(Document &doc) {
  std::set<std::string> names;
  walk(lxb_dom_interface_node(doc.tree), [&](lxb_dom_node_t *node) {
    if (!has_attr(node, "class")) return;
    std::string value = attr(node, "class");
    size_t pos = 0;
    while (pos < value.size()) {
      while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos]))) pos++;
      size_t end = pos;
      while (end < value.size() && !std::isspace(static_cast<unsigned char>(value[end]))) end++;
      if (end > pos) names.insert(value.substr(pos, end - pos));
      pos = end;
    }
  });
  return std::vector<std::string>(names.begin(), names.end());
}

std::vector<std::string> ids_used(Document &doc) {
  std::set<std::string> names;
  walk(lxb_dom_interface_node(doc.tree), [&](lxb_dom_node_t *node) {
    if (has_attr(node, "id")) names.insert(attr(node, "id"));
  });
  return std::vector<std::string>(names.begin(), names.end());
}

static std::string lex_str(const lexbor_str_t &s) {
  return s.data ? std::string(reinterpret_cast<const char *>(s.data), s.length) : "";
}

static void harvest_selector_list(const lxb_css_selector_list_t *list,
                                  std::set<std::string> &classes, std::set<std::string> &ids) {
  for (const lxb_css_selector_list_t *chain = list; chain; chain = chain->next)
    for (const lxb_css_selector_t *sel = chain->first; sel; sel = sel->next) {
      if (sel->type == LXB_CSS_SELECTOR_TYPE_CLASS) classes.insert(lex_str(sel->name));
      else if (sel->type == LXB_CSS_SELECTOR_TYPE_ID) ids.insert(lex_str(sel->name));
    }
}

void css_names(Document &doc, std::vector<std::string> &classes, std::vector<std::string> &ids) {
  std::set<std::string> class_set, id_set;
  lxb_css_parser_t *parser = lxb_css_parser_create();
  if (!parser || lxb_css_parser_init(parser, nullptr) != LXB_STATUS_OK)
    throw std::string("out of memory creating CSS parser");
  lxb_css_parser_selectors_init(parser);
  for (auto *node : select(lxb_dom_interface_node(doc.tree), "style")) {
    std::string css = text_content(node);
    lxb_css_stylesheet_t *sst = lxb_css_stylesheet_create(nullptr);
    if (!sst || lxb_css_stylesheet_parse(sst, parser,
                                         reinterpret_cast<const lxb_char_t *>(css.data()),
                                         css.size()) != LXB_STATUS_OK) {
      if (sst) lxb_css_stylesheet_destroy(sst, true);
      lxb_css_parser_selectors_destroy(parser);
      lxb_css_parser_destroy(parser, true);
      throw std::string("out of memory parsing inline stylesheet");
    }
    if (sst->root && sst->root->type == LXB_CSS_RULE_LIST) {
      auto *rules = reinterpret_cast<lxb_css_rule_list_t *>(sst->root);
      for (lxb_css_rule_t *rule = rules->first; rule; rule = rule->next)
        if (rule->type == LXB_CSS_RULE_STYLE)
          harvest_selector_list(lxb_css_rule_style(rule)->selector, class_set, id_set);
    }
    lxb_css_stylesheet_destroy(sst, true);
  }
  lxb_css_parser_selectors_destroy(parser);
  lxb_css_parser_destroy(parser, true);
  classes.assign(class_set.begin(), class_set.end());
  ids.assign(id_set.begin(), id_set.end());
}

void set_attr(lxb_dom_node_t *node, const std::string &name, const std::string &value) {
  lxb_dom_element_t *el = as_element(node);
  if (!el) throw std::string("set_attr on a non-element node");
  // A name the tokenizer could not produce would serialize as markup
  // (set_attr("x><script>...", "")) or as several attributes.
  if (name.empty()) throw std::string("empty attribute name");
  for (unsigned char c : name)
    if (c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '>' || c == '/' || c == '=' ||
        c == '<')
      throw std::string("invalid attribute name: " + name);
  if (!lxb_dom_element_set_attribute(el, reinterpret_cast<const lxb_char_t *>(name.data()),
                                     name.size(),
                                     reinterpret_cast<const lxb_char_t *>(value.data()),
                                     value.size()))
    throw std::string("set_attr failed");
}

void remove_node(lxb_dom_node_t *node) {
  if (!node->parent) throw std::string("cannot remove a node with no parent");
  lxb_dom_node_remove(node);
}

static lxb_dom_node_t *splice_fragment(lxb_dom_node_t *node, const std::string &html,
                                       bool before, bool replace) {
  lxb_dom_node_t *parent = node->parent;
  if (!parent) throw std::string("cannot splice at a node with no parent");
  // Fragments parsed in the <html> context come back as a new <head> and
  // <body>; next to the root they would replace the whole document. Splice
  // inside <head>/<body> instead.
  if (parent->type != LXB_DOM_NODE_TYPE_ELEMENT || tag_name(parent) == "html")
    throw std::string("cannot splice at <" + tag_name(node) +
                      ">: select a node inside <head> or <body>");
  lxb_dom_element_t *context = lxb_dom_interface_element(parent);
  check_depth_prescan(html, "fragment");
  size_t parent_depth = 0;
  for (lxb_dom_node_t *n = parent; n; n = n->parent) parent_depth++;
  lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
      lxb_html_interface_document(node->owner_document), context,
      reinterpret_cast<const lxb_char_t *>(html.data()), html.size());
  if (!frag || !frag->first_child) throw std::string("HTML fragment produced no nodes");
  // The tree must stay within MAX_NESTING (diff/hash recurse over it).
  std::vector<std::pair<lxb_dom_node_t *, size_t>> stack{{frag, 0}};
  while (!stack.empty()) {
    auto [n, depth] = stack.back();
    stack.pop_back();
    if (parent_depth + depth > MAX_NESTING + 1)
      throw std::string("fragment would nest deeper than " + std::to_string(MAX_NESTING));
    for (lxb_dom_node_t *c = n->first_child; c; c = c->next) stack.push_back({c, depth + 1});
  }
  lxb_dom_node_t *first = frag->first_child;
  lxb_dom_node_t *anchor = node;
  while (frag->first_child) {
    lxb_dom_node_t *child = frag->first_child;
    lxb_dom_node_remove(child); // lexbor's insert_* does not detach first
    if (before) lxb_dom_node_insert_before(node, child);
    else { lxb_dom_node_insert_after(anchor, child); anchor = child; }
  }
  if (replace) lxb_dom_node_remove(node);
  return first;
}

lxb_dom_node_t *replace_with_html(lxb_dom_node_t *node, const std::string &html) {
  return splice_fragment(node, html, true, true);
}

lxb_dom_node_t *insert_html(lxb_dom_node_t *node, const std::string &html, bool before) {
  return splice_fragment(node, html, before, false);
}

std::string document_html(Document &doc) {
  std::string out;
  if (lxb_html_serialize_tree_cb(lxb_dom_interface_node(doc.tree), append_cb, &out) !=
      LXB_STATUS_OK)
    throw std::string("serialization failed");
  return out;
}

std::string url_scheme(const std::string &url) {
  for (size_t i = 0; i < url.size(); i++) {
    char c = url[i];
    if (c == ':') return i ? url.substr(0, i) : "";
    if (c == '/' || c == '#' || c == '?') return "";
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (i && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
    if (!ok) return "";
  }
  return "";
}

std::string resolve_relative(const std::string &base_file, const std::string &ref) {
  std::string base;
  std::string::size_type slash = base_file.rfind('/');
  if (slash != std::string::npos) base = base_file.substr(0, slash);
  std::string joined = base.empty() || ref.rfind("/", 0) == 0 ? ref : base + "/" + ref;
  std::vector<std::string> kept;
  size_t pos = 0;
  while (pos <= joined.size()) {
    size_t next = joined.find('/', pos);
    std::string part = joined.substr(pos, next == std::string::npos ? next : next - pos);
    if (part == "..") {
      if (!kept.empty() && kept.back() != "..") kept.pop_back();
      else kept.push_back(part);
    } else if (!part.empty() && part != ".") {
      kept.push_back(part);
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  std::string out = ref.rfind("/", 0) == 0 ? "/" : "";
  for (size_t i = 0; i < kept.size(); i++) {
    if (i) out += "/";
    out += kept[i];
  }
  return out;
}

}
