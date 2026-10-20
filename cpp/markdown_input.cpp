#include "markdown_input.hpp"
#include "md4c/md4c-html.h"
#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <sstream>
namespace lmhtml {
namespace {
std::string trim(const std::string &s) {
  auto a=s.find_first_not_of(" \t\r\n");
  return a==std::string::npos ? "" : s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
std::string line(const std::string &s) {
  std::string out;
  for (unsigned char c:s) { if(c<=32) { if(!out.empty()&&out.back()!=' ') out+=' '; } else out+=c; }
  return trim(out);
}
std::string esc(const std::string &s) {
  std::string out;
  for(char c:s) { switch(c) {case '&': out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;case '\'':out+="&#39;";break;default:out+=c;} }
  return out;
}
std::string slug(const std::string &s) {
  std::string out;
  for(unsigned char c:s) {
    if(c>='A'&&c<='Z') c+=32;
    if((c>='a'&&c<='z')||(c>='0'&&c<='9')) out+=c;
    else if(!out.empty()&&out.back()!='-') out+='-';
  }
  if(!out.empty()&&out.back()=='-') out.pop_back();
  return out.empty()?"section":out;
}
std::string md_escape(const std::string &s) {
  std::string out;
  for(char c:line(s)) { if(std::string("\\`*_[]<>#!|~").find(c)!=std::string::npos) out+='\\'; out+=c; }
  return out;
}
std::string url(const std::string &s) {
  const char *hex="0123456789ABCDEF"; std::string out;
  for(unsigned char c:s) {
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='.'||c=='-'||c=='_') out+=c;
    else { out+='%';out+=hex[c>>4];out+=hex[c&15]; }
  }
  return out;
}
}
std::string markdown_shell(Document &doc, const std::string &shell, const std::string &path) {
  auto mains=select(lxb_dom_interface_node(doc.tree), "main");
  std::string content;
  if(!mains.empty()) for(auto *n=mains[0]->first_child;n;n=n->next) content+=outer_html(n);
  std::string toc="<ul>";
  for(auto *n:select(lxb_dom_interface_node(doc.tree), "main h2, main h3"))
    toc+="<li><a href=\"#"+esc(attr(n,"id"))+"\">"+esc(text_content(n))+"</a></li>";
  toc+="</ul>";
  const std::map<std::string,std::string> slots={{"title",esc(doc.title)},{"summary",esc(doc.summary)},{"content",content},{"toc",toc},{"path",esc(path)}};
  const std::string pattern=shell.empty()?"<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>{{title}}</title><meta name=\"description\" content=\"{{summary}}\"></head><body><main>{{content}}</main></body></html>\n":shell;
  std::string result; size_t pos=0;
  while(pos<pattern.size()) {
    auto a=pattern.find("{{",pos);
    if(a==std::string::npos) {result+=pattern.substr(pos);break;}
    result+=pattern.substr(pos,a-pos);auto b=pattern.find("}}",a+2);
    if(b==std::string::npos) throw std::string("unclosed template placeholder");
    auto name=pattern.substr(a+2,b-a-2);auto it=slots.find(name);
    if(it==slots.end()) throw std::string("unknown template placeholder: ")+name;
    result+=it->second;pos=b+2;
  }
  if(!shell.empty()&&shell.find("{{content}}") == std::string::npos) throw std::string("template requires {{content}}");
  return result;
}
std::unique_ptr<Document> from_markdown(const std::string &text, const std::string &path, bool allow_html, const std::string &shell) {
  if(!valid_utf8(text)) throw std::string("Markdown source is not valid UTF-8: ")+path;
  std::string body=text,title,summary;
  if(body.compare(0,3,"\xef\xbb\xbf")==0) body.erase(0,3);
  auto first=body.find('\n');
  if(trim(body.substr(0,first))=="---") {
    size_t pos=first==std::string::npos?body.size():first+1; bool closed=false;
    while(pos<body.size()) {
      auto end=body.find('\n',pos);if(end==std::string::npos)end=body.size();
      auto row=trim(body.substr(pos,end-pos));pos=end<body.size()?end+1:end;
      if(row=="---") {closed=true;break;}
      auto colon=row.find(':');if(colon==std::string::npos)continue;
      auto key=trim(row.substr(0,colon)),value=trim(row.substr(colon+1));
      if(value.size()>=2&&((value.front()=='"'&&value.back()=='"')||(value.front()=='\''&&value.back()=='\'')))value=value.substr(1,value.size()-2);
      if(key=="title")title=value;
      if(key=="summary")summary=value;
    }
    if(!closed) throw std::string("unterminated Markdown front matter: ")+path;
    body.erase(0,pos);
  }
  // The index marker is metadata, never enabled as arbitrary raw HTML.
  const std::string marker="<!-- generated-by: lmhtml index -->\n";
  if(body.rfind(marker,0)==0)body.erase(0,marker.size());
  if(body.size()>std::numeric_limits<MD_SIZE>::max()) throw std::string("Markdown input too large");
  struct Output {std::string text;bool failed=false;} output;
  auto callback=[](const MD_CHAR *s,MD_SIZE n,void *p) {auto &o=*static_cast<Output*>(p);try{o.text.append(s,n);}catch(...){o.failed=true;}};
  int status=md_html(body.data(),static_cast<MD_SIZE>(body.size()),callback,&output,MD_DIALECT_GITHUB|(allow_html?0:MD_FLAG_NOHTML),0);
  if(status||output.failed)throw std::string("Markdown conversion failed");
  auto doc=parse_html("<!doctype html><html><head></head><body><main>"+output.text+"</main></body></html>",path);
  auto *root=lxb_dom_interface_node(doc->tree);
  if(!allow_html) for(auto *n:select(root,"[class]")) {
    auto value=attr(n,"class");
    if(tag_name(n)=="code"&&value.rfind("language-",0)==0)set_attr(n,"data-language",value.substr(9));
    lxb_dom_element_remove_attribute(lxb_dom_interface_element(n),reinterpret_cast<const lxb_char_t*>("class"),5);
  }
  std::set<std::string> used;
  for(auto *n:select(root,"[id]"))used.insert(attr(n,"id"));
  for(auto *n:select(root,"h1,h2,h3,h4,h5,h6")) {
    if(has_attr(n,"id"))continue;
    auto base=slug(text_content(n)),id=base;unsigned i=2;
    while(used.count(id))id=base+"-"+std::to_string(i++);
    used.insert(id);set_attr(n,"id",id);
  }
  auto headings=select(root,"main h1"),paras=select(root,"main p");
  doc->title=line(title.empty()?(headings.empty()?std::filesystem::path(path).stem().string():text_content(headings[0])):title);
  doc->summary=line(summary.empty()?(paras.empty()?"":text_content(paras[0])):summary);
  auto html=markdown_shell(*doc,shell,path);
  auto result=parse_html(html,path);result->title=doc->title;result->summary=doc->summary;
  return result;
}
std::string markdown_index(const std::vector<Document *> &docs, const std::string &root, const std::string &order) {
  std::map<std::string,Document*> notes;
  for(auto *d:docs) {auto rel=std::filesystem::path(d->path).lexically_relative(root).generic_string();if(rel!="index.md")notes[rel]=d;}
  std::vector<std::string> names;std::set<std::string> seen;std::istringstream in(order);std::string name;
  while(std::getline(in,name,'\n')) {name=trim(name);if(name.empty())continue;if(!notes.count(name)||!seen.insert(name).second)throw std::string("unknown or duplicate index order entry: ")+name;names.push_back(name);}
  for(auto &item:notes)if(!seen.count(item.first))names.push_back(item.first);
  std::string out="<!-- generated-by: lmhtml index -->\n# Index\n\nRead the relevant chapter before changing this repository.\n\n";
  for(auto &key:names) {auto *d=notes.at(key);out+="- ["+md_escape(d->title)+"]("+url(key)+")";if(!d->summary.empty())out+=" — "+md_escape(d->summary);out+='\n';}
  return out;
}
}
