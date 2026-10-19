#include "html.hpp"
#include <functional>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

namespace lmhtml {

Document::~Document() {
  if (tree) lxb_html_document_destroy(tree);
}

std::unique_ptr<Document> parse_html(const std::string &html, const std::string &path) {
  auto doc = std::make_unique<Document>();
  doc->tree = lxb_html_document_create();
  if (!doc->tree) throw std::string("out of memory creating document");
  doc->path = path;
  lxb_status_t status = lxb_html_document_parse(
      doc->tree, reinterpret_cast<const lxb_char_t *>(html.data()), html.size());
  if (status != LXB_STATUS_OK) throw std::string("HTML parse failed for ") + path;
  return doc;
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
  if (lxb_html_serialize_cb(node, append_cb, &out) != LXB_STATUS_OK)
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
