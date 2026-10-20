#pragma once
#include "html.hpp"
namespace lmhtml {
std::unique_ptr<Document> from_markdown(const std::string &text, const std::string &path = "", bool allow_html = false, const std::string &shell = "");
std::string markdown_shell(Document &doc, const std::string &shell, const std::string &path);
std::string markdown_index(const std::vector<Document *> &docs, const std::string &root, const std::string &order);
}
