#pragma once
// Thin C++ wrapper over lexbor: RAII document, selector queries, attribute
// and text access, link collection, anchors, relative path resolution.
// All functions throw std::string on failure; nodes are owned by their
// Document and must not be freed or outlive it.
#include <lexbor/html/html.h>
#include <memory>
#include <string>
#include <vector>

namespace lmhtml {

struct Document {
  lxb_html_document_t *tree = nullptr;
  std::string path; // file it was loaded from; "" when parsed from a string
  Document() = default;
  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;
  ~Document();
};

// Host-side document limit: refuse pages nested deeper than this many
// elements (pathological nesting stalls the HTML parser for tens of seconds).
constexpr size_t MAX_NESTING = 512;

std::unique_ptr<Document> parse_html(const std::string &html, const std::string &path);
void check_depth_prescan(const std::string &html, const std::string &path);
void check_depth_dom(Document &doc);
std::vector<lxb_dom_node_t *> select(lxb_dom_node_t *scope, const std::string &css);
std::string tag_name(lxb_dom_node_t *node);
std::string attr(lxb_dom_node_t *node, const std::string &name); // "" when absent
bool has_attr(lxb_dom_node_t *node, const std::string &name);
std::string text_content(lxb_dom_node_t *node);
std::string outer_html(lxb_dom_node_t *node);
lxb_dom_node_t *parent_node(lxb_dom_node_t *node); // nullptr at the top
std::vector<std::string> collect_links(Document &doc); // a/link href + img/script src, tree order
bool has_anchor(Document &doc, const std::string &fragment);
std::vector<std::string> classes_used(Document &doc);  // sorted unique class tokens
std::vector<std::string> ids_used(Document &doc);      // sorted unique id values
// .class and #id names defined by the document's inline <style> sheets.
// v0: top-level style rules only (rules nested in @media are not harvested).
void css_names(Document &doc, std::vector<std::string> &classes, std::vector<std::string> &ids);

// Mutations (M5). All throw std::string on misuse; inserted nodes are owned
// by the document like any other node.
void set_attr(lxb_dom_node_t *node, const std::string &name, const std::string &value);
void remove_node(lxb_dom_node_t *node); // detach; memory stays with the document
// Parse an HTML fragment in the context of node's parent and splice it in.
// Return the first inserted node. Empty fragments are an error.
lxb_dom_node_t *replace_with_html(lxb_dom_node_t *node, const std::string &html);
lxb_dom_node_t *insert_html(lxb_dom_node_t *node, const std::string &html, bool before);
// Full document serialization (doctype included when present).
std::string document_html(Document &doc);
std::string url_scheme(const std::string &url); // "" for relative references
std::string resolve_relative(const std::string &base_file, const std::string &ref);

}
