#include "plain.hpp"
#include "markdown_input.hpp"
#include "markdown.hpp"
#include "md4c/md4c.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#include <process.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace lmhtml {
namespace {
std::string trim(std::string s) {auto a=s.find_first_not_of(" \r\n\t");return a==s.npos?"":s.substr(a,s.find_last_not_of(" \r\n\t")-a+1);}
std::string flat(const std::string &s) {return trim(std::regex_replace(s,std::regex("[ \\t\\r\\n]+")," "));}
bool member(const std::string &s,const std::string &list) {return list.find("|"+s+"|")!=list.npos;}
const std::string decoration="|strong|em|b|i|u|mark|small|span|font|s|strike|del|";
bool in_pre(lxb_dom_node_t *n) {for(;n;n=n->parent)if(tag_name(n)=="pre")return true;return false;}
bool drop_attr(const std::string &tag,const std::string &a) {return a=="class"||a=="style"||a.rfind("data-",0)==0||(a=="id"&&tag!="h2"&&tag!="h3");}
std::string decode(const std::string &s) {
  if(s.find('&')==s.npos)return s;
  auto d=parse_html("<body><span>"+s+"</span></body>","");return text_content(select(lxb_dom_interface_node(d->tree),"span")[0]);
}
std::string spaces(std::string s) {size_t p=0;while((p=s.find("\xc2\xa0",p))!=s.npos)s.replace(p,2," ");return s;}
struct Edit {size_t a,b;std::string value;};
std::string apply(std::string s,std::vector<Edit> e) {
  std::sort(e.begin(),e.end(),[](const Edit&a,const Edit&b){return a.a>b.a;});size_t last=s.size()+1;
  for(auto &v:e){if(v.b>last)throw std::string("overlapping normalization edits");s.replace(v.a,v.b-v.a,v.value);last=v.a;}return s;
}
std::string read(const std::string &p) {std::ifstream f(p,std::ios::binary);if(!f)throw std::string("cannot read: ")+p;return std::string(std::istreambuf_iterator<char>(f),{});}
std::string fetch_url(const std::string &url,std::string &effective) {
  if(url.rfind("https://",0)!=0&&url.rfind("http://",0)!=0)throw std::string("clean URL requires http or https");
  auto dir=std::filesystem::temp_directory_path()/("lmhtml-clean-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if(!std::filesystem::create_directory(dir))throw std::string("cannot create fetch temporary directory");
  struct Cleanup {std::filesystem::path dir;~Cleanup(){std::error_code e;std::filesystem::remove_all(dir,e);}} cleanup{dir};
  auto path=(dir/"page.html").string();
  const char *args[]={"curl","--fail","--location","--silent","--show-error","--max-time","60","--proto","=http,https","--output",path.c_str(),"--write-out","%{url_effective}",url.c_str(),nullptr};
  auto meta=(dir/"effective-url").string();
  auto *stream=std::fopen(meta.c_str(),"wb");if(!stream)throw std::string("cannot create fetch metadata");
  int status;
#ifdef _WIN32
  int original=_dup(_fileno(stdout));
  if(original<0||_dup2(_fileno(stream),_fileno(stdout))<0){std::fclose(stream);throw std::string("cannot redirect curl metadata");}
  status=static_cast<int>(_spawnvp(_P_WAIT,"curl",args));
  std::fflush(stdout);_dup2(original,_fileno(stdout));_close(original);
#else
  auto pid=fork();if(pid<0){std::fclose(stream);throw std::string("cannot start curl");}
  if(pid==0){if(dup2(fileno(stream),STDOUT_FILENO)<0)_exit(127);execvp("curl",const_cast<char *const *>(args));_exit(127);}
  if(waitpid(pid,&status,0)<0){std::fclose(stream);throw std::string("cannot wait for curl");}
#endif
  std::fclose(stream);
  effective=trim(read(meta));
  if(status!=0)throw std::string("URL fetch failed; curl must be installed (offline file extraction needs no dependencies)");
  return read(path);
}
std::vector<std::string> files(const std::vector<std::string> &paths) {
  namespace fs=std::filesystem;std::set<std::string> found;
  for(auto &p:paths){if(fs::is_directory(p)){for(auto &e:fs::recursive_directory_iterator(p)){if(e.is_symlink())continue;auto ext=e.path().extension().string();if(e.is_regular_file()&&(ext==".html"||ext==".htm"||ext==".md"))found.insert(e.path().generic_string());}}
  else if(fs::is_regular_file(p))found.insert(fs::path(p).generic_string());else throw std::string("missing path: ")+p;}
  return {found.begin(),found.end()};
}
std::string json(const std::string &s) {std::string o="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){o+='\\';o+=c;}else if(c=='\n')o+="\\n";else if(c=='\r')o+="\\r";else if(c=='\t')o+="\\t";else if(c<32)o+=' ';else o+=c;}return o+'"';}
std::string all_text(Document &d) {auto nodes=select(lxb_dom_interface_node(d.tree),"html");return nodes.empty()?"":text_content(nodes[0]);}
size_t words(const std::string &s) {std::istringstream in(s);std::string w;size_t n=0;while(in>>w)++n;return n;}
std::string absolute_url(const std::string &base,const std::string &url) {
  if(base.empty()||!url_scheme(url).empty())return url;
  auto sep=base.find("://");if(sep==base.npos)return resolve_relative(base,url);
  auto end=base.find('/',sep+3);auto origin=base.substr(0,end);auto scheme=base.substr(0,sep+1);
  if(url.rfind("//",0)==0)return scheme+url;
  auto path=end==base.npos?"/":base.substr(end);path=path.substr(0,path.find_first_of("?#"));
  if(url.empty())return base;
  if(url[0]=='#'||url[0]=='?')return origin+path+url;
  return origin+resolve_relative(path,url);
}
bool noise(lxb_dom_node_t *n) {
  auto tag=tag_name(n);if(member(tag,"|nav|header|footer|aside|script|style|form|noscript|template|button|input|select|textarea|"))return true;
  if(has_attr(n,"hidden")||attr(n,"aria-hidden")=="true")return true;
  auto role=attr(n,"role");if(member(role,"|navigation|banner|complementary|dialog|advertisement|"))return true;
  auto names=attr(n,"class")+" "+attr(n,"id");
  return std::regex_search(names,std::regex("(^|[ _-])(advertisement|advert|ads|cookie|consent|social-share|sidebar)([ _-]|$)",std::regex::icase));
}
std::string clean_node(lxb_dom_node_t *n,const std::string &base,bool pre=false) {
  if(n->type==LXB_DOM_NODE_TYPE_TEXT)return syntax_escape(pre?text_content(n):spaces(text_content(n)));
  if(n->type!=LXB_DOM_NODE_TYPE_ELEMENT)return "";
  if(noise(n))return "";
  auto tag=tag_name(n);std::string body;for(auto *c=n->first_child;c;c=c->next)body+=clean_node(c,base,pre||tag=="pre");
  if(member(tag,decoration)||(tag=="code"&&!pre))return body;
  if(!member(tag,"|section|article|h1|h2|h3|h4|h5|h6|p|ul|ol|li|dl|dt|dd|table|thead|tbody|tfoot|tr|td|th|pre|code|kbd|samp|a|img|blockquote|hr|br|figure|figcaption|video|source|"))return body;
  std::string attrs;
  for(auto &a:attribute_names(n)) {
    if((tag=="a"&&a=="href")||((tag=="img"||tag=="source"||tag=="video")&&a=="src")) {
      auto u=attr(n,a);auto scheme=url_scheme(u);if(!scheme.empty()&&!member(scheme,"|http|https|mailto|tel|"))continue;
      attrs+=" "+a+"=\""+syntax_escape(absolute_url(base,u),true)+"\"";
    } else if((tag=="img"&&a=="alt")||((tag=="th"||tag=="td")&&member(a,"|colspan|rowspan|scope|"))||(tag=="ol"&&a=="start"))attrs+=" "+a+"=\""+syntax_escape(attr(n,a),true)+"\"";
  }
  if(member(tag,"|img|hr|br|source|"))return "<"+tag+attrs+">";
  return "<"+tag+attrs+">"+body+"</"+tag+">";
}
struct MDStats {size_t emphasis=0,code=0,html=0,wrap=0;};
MDStats md_stats(std::string text) {
  const std::string marker="<!-- generated-by: lmhtml index -->\n";if(text.rfind(marker,0)==0)text.erase(0,marker.size());
  if(text.rfind("---\n",0)==0){auto end=text.find("\n---",4);if(end!=text.npos)text.erase(0,end+4);}
  MDStats result;MD_PARSER p{};p.flags=MD_DIALECT_GITHUB;
  p.enter_block=[](MD_BLOCKTYPE,void*,void*){return 0;};p.leave_block=p.enter_block;
  p.enter_span=[](MD_SPANTYPE t,void*,void *v){auto &r=*static_cast<MDStats*>(v);if(t==MD_SPAN_EM||t==MD_SPAN_STRONG||t==MD_SPAN_DEL)++r.emphasis;if(t==MD_SPAN_CODE)++r.code;return 0;};
  p.leave_span=[](MD_SPANTYPE,void*,void*){return 0;};
  p.text=[](MD_TEXTTYPE t,const MD_CHAR*,MD_SIZE,void *v){auto &r=*static_cast<MDStats*>(v);if(t==MD_TEXT_HTML)++r.html;if(t==MD_TEXT_SOFTBR)++r.wrap;return 0;};
  if(md_parse(text.data(),static_cast<MD_SIZE>(text.size()),&p,&result))throw std::string("Markdown lint parse failed");
  return result;
}
}
std::string syntax_escape(const std::string &s,bool attribute) {
  std::string o;for(char c:s){if(c=='&')o+="&amp;";else if(!attribute&&c=='<')o+="&lt;";else if(attribute&&c=='"')o+="&quot;";else o+=c;}return o;
}
std::string normalize_html(const std::string &s) {
  auto d=parse_html(s,"");if(!d->utf8_source)throw std::string("normalize requires UTF-8");
  std::vector<Edit> edits;std::vector<std::pair<size_t,size_t>> tags;
  for(auto &r:d->source_ranges) {
    if(r.tag.empty()||r.tag[0]=='!'||r.tag[0]=='?') {tags.push_back({r.begin,r.end});continue;}
    bool pre=r.tag=="pre";auto parent=r.parent;while(parent!=std::string::npos){auto &p=d->source_ranges[parent];pre=pre||p.tag=="pre";parent=p.parent;}
    bool empty=member(r.tag,"|p|li|dt|dd|")&&trim(s.substr(r.inner_begin,r.inner_end-r.inner_begin)).empty();
    bool unwrap=member(r.tag,decoration)||(r.tag=="code"&&!pre)||empty;
    tags.push_back({r.begin,r.inner_begin});if(r.inner_end<r.end)tags.push_back({r.inner_end,r.end});
    auto opening=s.substr(r.begin,r.inner_begin-r.begin);
    if(unwrap){edits.push_back({r.begin,r.inner_begin,""});if(r.inner_end<r.end)edits.push_back({r.inner_end,r.end,""});continue;}
    // Use the tokenizer's source span and Lexbor's attribute parsing, preserving
    // the rest of the input byte-for-byte instead of reserializing the document.
    auto neutral="<plain-node"+opening.substr(1+r.tag.size());
    auto frag=parse_html("<body>"+neutral+"</plain-node>","");auto nodes=select(lxb_dom_interface_node(frag->tree),"plain-node");
    // Context-sensitive tags (td/head/etc.) may be repaired out of fragments.
    if(nodes.empty())continue;
    auto *n=nodes.back();std::string canonical="<"+r.tag;
    for(auto &a:attribute_names(n))if(!drop_attr(r.tag,a))canonical+=" "+a+"=\""+syntax_escape(spaces(attr(n,a)),true)+"\"";
    canonical+='>';
    bool change=std::any_of(frag->markup_errors.begin(),frag->markup_errors.end(),[](const std::string &e){return e.rfind("duplicate attribute",0)==0;});for(auto &a:attribute_names(n))if(drop_attr(r.tag,a))change=true;
    if(opening.find('&')!=opening.npos||change)edits.push_back({r.begin,r.inner_begin,canonical});
  }
  std::sort(tags.begin(),tags.end());size_t pos=0;
  auto text=[&](size_t a,size_t b){if(a>=b)return;auto part=s.substr(a,b-a);if(part.find("<!--")!=part.npos||part.find("<!doctype")!=part.npos||part.find("<!DOCTYPE")!=part.npos)return;
    bool pre=false,raw=false,prose=false;for(auto &r:d->source_ranges)if(a>=r.inner_begin&&b<=r.inner_end){pre|=r.tag=="pre";raw|=r.tag=="script"||r.tag=="style";prose|=member(r.tag,"|p|li|dt|dd|h1|h2|h3|h4|td|th|");}
    if(raw)return;
    auto value=syntax_escape(spaces(decode(part)));if(!pre&&prose)value=std::regex_replace(value,std::regex("[ \\t]*[\\r\\n]+[ \\t]*")," ");
    if(value!=part)edits.push_back({a,b,value});};
  for(auto &r:tags){text(pos,r.first);pos=std::max(pos,r.second);}text(pos,s.size());
  auto out=apply(s,edits);
  // Whitespace-only line tails have no semantic value, except within code.
  auto after=parse_html(out,"");std::vector<Edit> tails;size_t start=0;
  while(start<out.size()){auto end=out.find('\n',start);if(end==out.npos)end=out.size();auto tail=end;while(tail>start&&(out[tail-1]==' '||out[tail-1]=='\t'||out[tail-1]=='\r'))--tail;
    bool pre=false;for(auto &r:after->source_ranges)if(r.tag=="pre"&&tail>=r.inner_begin&&end<=r.inner_end)pre=true;
    bool code=false;for(auto &r:after->source_ranges)if(member(r.tag,"|code|kbd|samp|")&&tail>=r.inner_begin&&end<=r.inner_end)code=true;
    if(pre&&!code&&tail>0&&out[tail-1]=='>')pre=false;
    if(tail<end&&!pre)tails.push_back({tail,end,""});
    start=end+1;}
  out=apply(out,tails);
  auto final=parse_html(out,"");if(flat(spaces(all_text(*d)))!=flat(spaces(all_text(*final)))){auto a=flat(spaces(all_text(*d))),b=flat(spaces(all_text(*final)));size_t k=0;while(k<a.size()&&k<b.size()&&a[k]==b[k])++k;throw std::string("normalize refused: text content changed near ")+a.substr(k,100)+" -> "+b.substr(k,100);}
  return out;
}
std::string clean_html(const std::string &s,const std::string &base) {
  auto d=parse_html(s,"");auto root=lxb_dom_interface_node(d->tree);auto nodes=select(root,"main");if(nodes.empty())nodes=select(root,"[role=main]");if(nodes.empty())nodes=select(root,"article");if(nodes.empty())nodes=select(root,"body");
  if(nodes.empty())return "";
  std::string out;for(auto *c=nodes[0]->first_child;c;c=c->next)out+=clean_node(c,base);
  return trim(normalize_html(out))+"\n";
}
std::string plain_markdown(const std::string &html) {
  auto d=parse_html("<main>"+clean_html(html)+"</main>","");
  // dt carries meaning through the definition list, not through bold type.
  for(auto *n:select(lxb_dom_interface_node(d->tree),"dt"))replace_with_html(n,"<p>"+syntax_escape(text_content(n))+"</p>");
  auto raw=document_markdown(*d,"main");
  std::istringstream lines(raw);std::string line,out;bool fence=false;
  while(std::getline(lines,line)) {
    if(line.rfind("```",0)==0)fence=!fence;
    if(!fence&&line.rfind("```",0)!=0) {
      std::string plain;
      for(size_t i=0;i<line.size();++i) {
        if(line[i]=='\\'&&i+1<line.size()&&std::string(".-)!+#").find(line[i+1])!=std::string::npos) {
          auto prefix=trim(plain);bool block=prefix.empty()||std::all_of(prefix.begin(),prefix.end(),[](char c){return c>='0'&&c<='9';});
          if(!block){plain+=line[++i];continue;}
        }
        if(line[i]=='\\'&&i>0&&i+2<line.size()&&line[i+1]=='_'&&std::isalnum(static_cast<unsigned char>(line[i-1]))&&std::isalnum(static_cast<unsigned char>(line[i+2]))){plain+=line[++i];continue;}
        plain+=line[i];
      }
      line=plain;
    }
    out+=line+'\n';
  }
  return out;
}
std::string normalize_markdown(const std::string &s) {
  std::string prefix,body=s;
  if(body.rfind("---\n",0)==0){auto p=body.find("\n---",4);if(p==body.npos)throw std::string("unterminated front matter");p=body.find('\n',p+1);if(p==body.npos)p=body.size();else ++p;prefix=body.substr(0,p);body.erase(0,p);}
  const std::string marker="<!-- generated-by: lmhtml index -->\n";if(body.rfind(marker,0)==0){prefix+=marker;body.erase(0,marker.size());}
  auto d=from_markdown(body);auto out=prefix+plain_markdown(d->source);
  auto check=from_markdown(out);
  if(flat(text_content(select(lxb_dom_interface_node(d->tree),"main")[0]))!=flat(text_content(select(lxb_dom_interface_node(check->tree),"main")[0])))throw std::string("Markdown normalize refused: text content changed");
  return out;
}
PlainStats plain_stats(const std::string &s,const std::string &path,bool markdown) {
  PlainStats st;st.path=path;st.bytes=s.size();auto d=markdown?from_markdown(s):parse_html(s,path);auto root=lxb_dom_interface_node(d->tree);st.words=words(all_text(*d));
  for(auto &key:{"strong","em","b","i","u","mark","small","span","font","s","strike","del","inline_code","class","style","id","data","md_emphasis","md_inline_code","raw_html","hard_wrap","trailing_whitespace","emoji","empty","duplicate_attribute"})st.counts[key]=0;
  if(markdown){auto m=md_stats(s);st.counts["md_emphasis"]=m.emphasis;st.counts["md_inline_code"]=m.code;st.counts["raw_html"]=m.html;st.counts["hard_wrap"]=m.wrap;
    if(select(root,"main h1").size()!=1)st.findings.push_back("Markdown notes require exactly one h1");
  }else for(auto *n:select(root,"*")){
    auto tag=tag_name(n);if(member(tag,"|p|li|dt|dd|")&&trim(text_content(n)).empty()&&select(n,"img,video,hr,br").empty())++st.counts["empty"];if(member(tag,decoration))++st.counts[tag];if(tag=="code"&&!in_pre(n))++st.counts["inline_code"];
    for(auto &a:attribute_names(n)){if(member(a,"|class|style|id|"))++st.counts[a];if(a.rfind("data-",0)==0)++st.counts["data"];if(a=="id"&&tag!="h2"&&tag!="h3")st.findings.push_back(node_path(n)+": id allowed only on h2/h3");}
    if(member(tag,"|p|li|dt|dd|h1|h2|h3|h4|")&&text_content(n).find('\n')!=s.npos&&!in_pre(n)&&select(n,"pre,ul,ol,dl").empty())++st.counts["hard_wrap"];
  }
  for(auto &e:d->markup_errors)if(e.rfind("duplicate attribute",0)==0)++st.counts["duplicate_attribute"];
  // Count every entity by spelling; only canonical syntax escapes are allowed.
  std::regex ent("&(#x[0-9A-Fa-f]+|#[0-9]+|[A-Za-z][A-Za-z0-9]+);");
  for(auto it=std::sregex_iterator(s.begin(),s.end(),ent);it!=std::sregex_iterator();++it){auto value=it->str();bool attribute=false;size_t p=static_cast<size_t>(it->position());
    bool comment=false;
    if(!markdown)for(auto &r:d->source_ranges) {
      if(p>=r.begin&&p<r.end&&!r.tag.empty()&&r.tag[0]=='!')comment=true;
      if(p>=r.begin&&p<r.inner_begin)attribute=true;
    }
    if(comment)continue;
    if(value=="&amp;"||(!attribute&&value=="&lt;")||(attribute&&value=="&quot;"))continue;
    ++st.counts["entity:"+value];}
  std::istringstream lines(s);std::string row;while(std::getline(lines,row))if(!row.empty()&&(row.back()==' '||row.back()=='\t'||row.back()=='\r'))++st.counts["trailing_whitespace"];
  for(size_t p=0;p<s.size();){unsigned char c=s[p++];unsigned cp=c;int n=0;if(c>=0xf0){cp=c&7;n=3;}else if(c>=0xe0){cp=c&15;n=2;}else if(c>=0xc0){cp=c&31;n=1;}for(int i=0;i<n&&p<s.size();++i)cp=(cp<<6)|(s[p++]&63);if((cp>=0x1f000&&cp<=0x1faff)||(cp>=0x2600&&cp<=0x27bf)||cp==0xfe0f||cp==0x20e3||cp==0x2b1b||cp==0x2b1c||cp==0x2b50||cp==0x2b55||cp==0x231a||cp==0x231b||(cp>=0x23e9&&cp<=0x23f3)||(cp>=0x23f8&&cp<=0x23fa)||(cp>=0xe0020&&cp<=0xe007f))++st.counts["emoji"];}
  return st;
}
int plain_cli(int argc,char **argv,const std::function<void(const std::string &,const std::string &)> &save) {
  std::string command=argv[1],base,compare,config;bool stats=false,j=false,check=false,mdout=false;std::vector<std::string> paths;
  for(int i=2;i<argc;++i){std::string a=argv[i];if(a=="--stats")stats=true;else if(a=="--json")j=true;else if(a=="--check")check=true;else if(a=="--markdown")mdout=true;else if((a=="--base"||a=="--compare"||a=="--config")&&i+1<argc){auto v=argv[++i];if(a=="--base")base=v;else if(a=="--compare")compare=v;else config=v;}else if(a.rfind("--",0)==0)throw std::string("unknown/incomplete option: ")+a;else paths.push_back(a);}
  if(paths.empty())throw std::string("expected PATHS");
  if(command=="clean"){
    if(paths.size()!=1)throw std::string("clean expects one URL or file");
    bool remote=paths[0].rfind("https://",0)==0||paths[0].rfind("http://",0)==0;
    std::string effective;auto source=remote?fetch_url(paths[0],effective):read(paths[0]);if(remote&&base.empty())base=effective;auto out=mdout?plain_markdown(clean_html(source,base)):clean_html(source,base);
    std::cerr<<"clean: bytes "<<source.size()<<" -> "<<out.size()<<", estimated tokens "<<(source.size()+3)/4<<" -> "<<(out.size()+3)/4<<" (bytes/4)\n";
    if(check){auto d=parse_html(source,"");auto mains=select(lxb_dom_interface_node(d->tree),"main");if(mains.size()!=1)throw std::string("clean check requires one main");
      // Compare main's serialized semantic content to extraction, allowing
      // only attribute removal and URL resolution, not removal of text/nodes.
      for(auto *n:select(mains[0],"*")) {
        auto tag=tag_name(n);
        for(auto &attrname:attribute_names(n)) {
          bool url=(tag=="a"&&attrname=="href")||((tag=="img"||tag=="video"||tag=="source")&&attrname=="src");
          bool keep=url||(tag=="img"&&attrname=="alt")||((tag=="th"||tag=="td")&&member(attrname,"|scope|colspan|rowspan|"))||(tag=="ol"&&attrname=="start");
          if(!keep)lxb_dom_element_remove_attribute(lxb_dom_interface_element(n),reinterpret_cast<const lxb_char_t*>(attrname.data()),attrname.size());
          else if(url)set_attr(n,attrname,absolute_url(base,attr(n,attrname)));
        }
      }
      std::function<void(lxb_dom_node_t*)> comments=[&](lxb_dom_node_t *parent){for(auto *n=parent->first_child;n;){auto *next=n->next;if(n->type==LXB_DOM_NODE_TYPE_COMMENT)remove_node(n);else comments(n);n=next;}};
      comments(mains[0]);
      std::string expected;for(auto *n=mains[0]->first_child;n;n=n->next)if(n->type!=LXB_DOM_NODE_TYPE_COMMENT)expected+=outer_html(n);
      expected=trim(expected)+"\n";
      auto cleaned=clean_html(source,base);
      if(expected!=cleaned)throw std::string("clean check differs from main; expected:\n")+expected+"extracted:\n"+cleaned;
      return 0;}
    std::cout<<out;return 0;
  }
  auto list=files(paths);
  if(command=="normalize") {bool changed=false;for(auto &p:list){auto s=read(p);auto out=std::filesystem::path(p).extension()==".md"?normalize_markdown(s):normalize_html(s);if(s!=out){changed=true;std::cerr<<p<<": normalize"<<(check?" required":" applied")<<"\n";if(!check)save(out,p);}}return check&&changed?1:0;}
  std::map<std::string,std::map<std::string,size_t>> limits;
  if(!config.empty()){std::istringstream in(read(config));std::string row;while(std::getline(in,row)){if(trim(row).empty()||trim(row)[0]=='#')continue;std::istringstream r(row);std::string dir,key;size_t n;if(!(r>>dir>>key>>n))throw std::string("config: expected DIRECTORY METRIC MAX_COUNT");limits[std::filesystem::absolute(dir).lexically_normal().generic_string()][key]=n;}}
  std::vector<PlainStats> reports;PlainStats total;total.path="TOTAL";
  for(auto &p:list){auto s=read(p);auto r=plain_stats(s,p,std::filesystem::path(p).extension()==".md");
    for(auto &c:r.counts){size_t limit=0;auto absolute=std::filesystem::absolute(p).lexically_normal().generic_string();
      for(auto &rule:limits)if((absolute==rule.first||absolute.rfind(rule.first+"/",0)==0)&&rule.second.count(c.first))limit=rule.second.at(c.first);
      if(c.first!="id"&&c.second>limit)r.findings.push_back(c.first+": "+std::to_string(c.second)+" (limit "+std::to_string(limit)+")");
      total.counts[c.first]+=c.second;}
    total.bytes+=r.bytes;total.words+=r.words;total.findings.insert(total.findings.end(),r.findings.begin(),r.findings.end());reports.push_back(r);}
  std::sort(reports.begin(),reports.end(),[](const PlainStats&a,const PlainStats&b){size_t x=0,y=0;for(auto &c:a.counts)x+=c.second;for(auto &c:b.counts)y+=c.second;double da=100.0*x/std::max(size_t(1),a.words),db=100.0*y/std::max(size_t(1),b.words);return da==db?a.path<b.path:da>db;});
  reports.push_back(total);
  if(j)std::cout<<"{\"files\":[";
  for(size_t i=0;i<reports.size();++i){auto &r=reports[i];if(j){if(i)std::cout<<',';std::cout<<"{\"path\":"<<json(r.path)<<",\"bytes\":"<<r.bytes<<",\"words\":"<<r.words<<",\"estimated_tokens\":"<<(r.bytes+3)/4<<",\"counts\":{";bool first=true;for(auto &c:r.counts){if(!first)std::cout<<',';first=false;std::cout<<json(c.first)<<':'<<c.second;}std::cout<<"},\"per_100_words\":{";first=true;for(auto &c:r.counts){if(!first)std::cout<<',';first=false;std::cout<<json(c.first)<<':'<<100.0*c.second/std::max(size_t(1),r.words);}std::cout<<"},\"findings\":[";for(size_t k=0;k<r.findings.size();++k){if(k)std::cout<<',';std::cout<<json(r.findings[k]);}std::cout<<"]}";}
    else if(stats||!r.findings.empty()){std::cout<<r.path<<": "<<r.bytes<<" bytes, "<<r.words<<" words, "<<(r.bytes+3)/4<<" estimated tokens\n";for(auto &c:r.counts)if(c.second)std::cout<<"  "<<c.first<<"="<<c.second<<" ("<<100.0*c.second/std::max(size_t(1),r.words)<<"/100 words)\n";if(r.path!="TOTAL")for(auto &f:r.findings)std::cout<<"  FAIL "<<f<<'\n';}}
  if(j)std::cout<<']';
  if(!compare.empty()){
    PlainStats old;for(auto &p:files({compare})){auto r=plain_stats(read(p),p,std::filesystem::path(p).extension()==".md");old.bytes+=r.bytes;old.words+=r.words;for(auto &c:r.counts)old.counts[c.first]+=c.second;}
    if(j)std::cout<<",\"delta\":{\"bytes\":"<<static_cast<long long>(total.bytes)-static_cast<long long>(old.bytes)<<",\"counts\":{";else std::cout<<"CHANGE bytes="<<static_cast<long long>(total.bytes)-static_cast<long long>(old.bytes)<<'\n';bool first=true;std::set<std::string> keys;for(auto &c:old.counts)keys.insert(c.first);for(auto &c:total.counts)keys.insert(c.first);for(auto &k:keys){auto delta=static_cast<long long>(total.counts[k])-static_cast<long long>(old.counts[k]);if(j){if(!first)std::cout<<',';first=false;std::cout<<json(k)<<':'<<delta;}else if(delta)std::cout<<"  "<<k<<"="<<delta<<'\n';}if(j)std::cout<<"}}";
  }
  if(j)std::cout<<"}\n";
  return total.findings.empty()?0:1;
}
}
