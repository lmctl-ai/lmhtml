#pragma once
#include "html.hpp"
#include <algorithm>

namespace lmhtml {
namespace markdown_detail {
inline std::string trim(std::string s) {
  auto a=s.find_first_not_of(" \t\r\n");
  return a==std::string::npos ? "" : s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline std::string escape(const std::string &s) {
  std::string out;
  for(char c:s) {
    if(c=='\r'||c=='\n'||c=='\t'||c==' ') { if(out.empty()||out.back()!=' ') out+=' '; }
    else { if(std::string("\\`*_[]<>#!|~").find(c)!=std::string::npos) out+='\\'; out+=c; }
  }
  return out;
}
inline std::string ticks(const std::string &s, size_t minimum) {
  size_t longest=0, run=0;
  for(char c:s) { run=c=='`'?run+1:0; longest=std::max(longest,run); }
  return std::string(std::max(minimum,longest+1),'`');
}
inline std::string target(std::string s) {
  std::string out;
  for(unsigned char c:s) {
    if(c<=32||c=='<'||c=='>'||c=='\\') { const char *hex="0123456789ABCDEF"; out+='%'; out+=hex[c>>4]; out+=hex[c&15]; }
    else out+=static_cast<char>(c);
  }
  return "<"+out+">";
}
inline std::string render(lxb_dom_node_t *n);
inline std::string children(lxb_dom_node_t *n) {
  std::string s;
  for(auto *c=n->first_child;c;c=c->next) s+=render(c);
  return s;
}
inline std::string indent(const std::string &s,const std::string &prefix) {
  std::string out=prefix;
  for(size_t i=0;i<s.size();++i) { out+=s[i]; if(s[i]=='\n'&&i+1<s.size()) out+=prefix; }
  return out;
}
inline std::string render(lxb_dom_node_t *n) {
  if(n->type==LXB_DOM_NODE_TYPE_TEXT) return escape(text_content(n));
  if(n->type==LXB_DOM_NODE_TYPE_COMMENT) return "";
  if(n->type!=LXB_DOM_NODE_TYPE_ELEMENT) return children(n);
  auto tag=tag_name(n);
  if(tag=="pre") {
    auto s=text_content(n); auto fence=ticks(s,3);
    return "\n\n"+fence+"\n"+s+(s.empty()||s.back()!='\n'?"\n":"")+fence+"\n\n";
  }
  if(tag=="code"||tag=="kbd"||tag=="samp") {
    auto s=text_content(n);
    for(char &c:s) if(c=='\r'||c=='\n') c=' ';
    auto fence=ticks(s,1);
    bool pad=!s.empty()&&(s.front()=='`'||s.back()=='`'||(s.front()==' '&&s.back()==' '&&s.find_first_not_of(' ')!=std::string::npos));
    return fence+(pad?" ":"")+s+(pad?" ":"")+fence;
  }
  if(tag=="ul"||tag=="ol") {
    std::string out="\n\n"; int number=1;
    if(has_attr(n,"start")) {
      try { size_t end=0; number=std::stoi(attr(n,"start"),&end); if(end!=attr(n,"start").size()||number<0) throw 0; }
      catch(...) { throw std::string("markdown: unsupported ordered-list start"); }
    }
    for(auto *c=n->first_child;c;c=c->next) if(c->type==LXB_DOM_NODE_TYPE_ELEMENT) {
      if(tag_name(c)!="li") throw std::string("markdown: list child must be li");
      auto prefix=tag=="ul"?"- ":std::to_string(number)+". ";
      auto body=trim(children(c));
      out+=prefix+indent(body,std::string(prefix.size(),' ')).substr(prefix.size())+"\n";
      if(tag=="ol"&&c->next) { if(number==2147483647) throw std::string("markdown: list number overflow"); ++number; }
    }
    return out+"\n";
  }
  if(tag=="table") {
    auto rows=select(n,"tr"); std::string out="\n\n"; size_t width=0;
    for(size_t i=0;i<rows.size();++i) {
      auto cells=select(rows[i],"th, td");
      if(i==0) width=cells.size();
      if(cells.size()!=width||width==0) throw std::string("markdown: rectangular table required");
      out+='|';
      for(auto *cell:cells) {
        if(has_attr(cell,"colspan")||has_attr(cell,"rowspan")) throw std::string("markdown: table spans unsupported");
        auto content=trim(children(cell));
        std::string escaped;
        for(size_t k=0;k<content.size();++k) {
          char c=content[k];
          if(c=='\n') throw std::string("markdown: block content in table cell unsupported");
          if(c=='|'&&(k==0||content[k-1]!='\\')) escaped+='\\';
          escaped+=c;
        }
        out+=" "+escaped+" |";
      }
      out+='\n';
      if(i==0) { out+='|'; for(size_t j=0;j<width;++j) out+=" --- |"; out+='\n'; }
    }
    return out+"\n";
  }
  auto s=children(n);
  if(tag=="h1"||tag=="h2"||tag=="h3"||tag=="h4") return "\n\n"+std::string(tag[1]-'0','#')+" "+trim(s)+"\n\n";
  if(tag=="p"||tag=="figure"||tag=="figcaption"||tag=="dd") return "\n\n"+trim(s)+"\n\n";
  if(tag=="strong"||tag=="b"||tag=="dt") return "**"+trim(s)+"**";
  if(tag=="em"||tag=="i") return "*"+trim(s)+"*";
  if(tag=="blockquote") return "\n\n"+indent(trim(s),"> ")+"\n\n";
  if(tag=="hr") return "\n\n---\n\n";
  if(tag=="br") return "  \n";
  if(tag=="a"||tag=="img") {
    std::string title=attr(n,"title");
    std::string quoted;
    for(char c:title) { if(c=='"'||c=='\\') quoted+='\\'; quoted+=c; }
    return (tag=="img"?"![":"[")+(tag=="img"?escape(attr(n,"alt")):s)+"]("+target(attr(n,tag=="img"?"src":"href"))+(title.empty()?"":" \""+quoted+"\"")+")";
  }
  if(tag=="main"||tag=="body"||tag=="section"||tag=="article"||tag=="dl"||tag=="li") return s;
  throw std::string("markdown: unsupported element <"+tag+">");
}
}
// Explicit selection keeps document chrome out of exported content. Unsupported
// structures fail rather than silently lose authored material.
inline std::string document_markdown(Document &doc,const std::string &selector) {
  if(!doc.utf8_source) throw std::string("markdown: UTF-8 source required");
  auto nodes=select(lxb_dom_interface_node(doc.tree),selector);
  if(nodes.size()!=1) throw std::string("markdown: selector must match exactly one element");
  return markdown_detail::trim(markdown_detail::render(nodes[0]))+"\n";
}
}
