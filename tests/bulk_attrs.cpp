// Direct native callbacks let the test inspect state after a rejected batch.
#define main lmhtml_application_main
#include "../cpp/main.cpp"
#undef main
#include <iostream>
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void atomic_bad_second_key_leaves_everything_unchanged(dslvm::Registry& registry,lxb_dom_node_t* node,lmhtml::Document& doc){
 dslvm::stdlib::StrMap bad;bad.set("title","changed");bad.set("bad key","x");Args args{Value::object("StrMap",&bad)};
 const auto before=lmhtml::document_html(doc);bool rejected=false;try{registry.find_method("Node","set_attrs")->fn(node,args);}catch(...){rejected=true;}
 check(rejected,"atomic bad second key accepted");check(lmhtml::document_html(doc)==before,"atomic bad second key changed DOM");
 std::cout<<"PASS atomic_bad_second_key_leaves_everything_unchanged\n";
}
int main(int argc,char** argv){try{
 Host host;dslvm::Registry registry;register_host(registry,host);
 auto* setter=registry.find_method("Node","set_attrs");auto* getter=registry.find_method("Node","get_attrs");
 check(setter&&getter,"bulk attribute methods are not registered");
 auto doc=lmhtml::parse_html("<div id='n' title='original'></div>","");auto nodes=lmhtml::select(lxb_dom_interface_node(doc->tree),"#n");check(nodes.size()==1,"fixture node");auto* node=nodes[0];
 atomic_bad_second_key_leaves_everything_unchanged(registry,node,*doc);
 if(argc==2&&std::string(argv[1])=="--atomic-bad-second-key")return 0;
 Args args{Value::object("StrMap",nullptr)};
 dslvm::stdlib::StrMap good;good.set("data-empty","");good.set("title","α & quoted");good.set("data-large",std::string(70000,'x'));args[0]=Value::object("StrMap",&good);check(setter->fn(node,args).obj==node,"setter lost receiver");
 dslvm::stdlib::StrList keys;keys.items={"data-empty","title","absent","data-large"};Args query{Value::object("StrList",&keys)};
 auto result=getter->fn(node,query);auto* map=static_cast<dslvm::stdlib::StrMap*>(result.obj);auto entries=map->entries();check(entries.size()==3,"getter absent/present keys");check(entries[0].second.empty(),"empty value lost");check(entries[1].second=="α & quoted","Unicode value changed");check(entries[2].second.size()==70000,"value clipped");map->set("title","copy edit");check(lmhtml::attr(node,"title")=="α & quoted","getter aliases native storage");
 check(bool(setter->dry_fn)&&bool(getter->dry_fn),"bulk dry callbacks absent");const auto dry_before=lmhtml::document_html(*doc);setter->dry_fn(node,args);check(lmhtml::document_html(*doc)==dry_before,"dry setter mutated node");check(setter->dry_fn(nullptr,args).obj==nullptr,"dry null receiver");check(getter->dry_fn(nullptr,query).obj!=nullptr,"dry getter needs a fresh map");
 std::cout<<"PASS bulk attrs: atomic keys, round-trip copy, Unicode/empty/large, dry/null\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}catch(const std::string& e){std::cerr<<e<<"\n";return 1;}}
