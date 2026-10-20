#pragma once
#include "html.hpp"
#include <map>
namespace lmhtml {
std::string syntax_escape(const std::string &, bool attribute=false);
std::string normalize_html(const std::string &);
std::string normalize_markdown(const std::string &);
std::string clean_html(const std::string &, const std::string &base="");
std::string plain_markdown(const std::string &);
struct PlainStats {
  std::string path;
  size_t bytes=0,words=0;
  std::map<std::string,size_t> counts;
  std::vector<std::string> findings;
};
PlainStats plain_stats(const std::string &,const std::string &,bool markdown);
int plain_cli(int argc,char **argv,const std::function<void(const std::string &,const std::string &)> &save);
}
