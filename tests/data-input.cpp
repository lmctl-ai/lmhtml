#define main data_input_example_main
#include "../examples/data-input/host.cpp"
#undef main
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
 if(argc!=2)return 2;const std::string target=argv[1];std::filesystem::remove(target);
 dslvm::Registry r;data_input::Counts counts;data_input::register_input(r,counts,target);
 const auto* f=r.function_by_signature("fn accept_attributes(string, string) -> bool");check(f,"sample registration missing");check(bool(f->dry_fn),"dry callback missing");
 auto scope=r.std_run_scope();dslvm::Args a{dslvm::Value::string("json"),dslvm::Value::string("{\"title\":\"water 水\",\"data-x\":\"\"}")};
 check(f->dry_fn(a).b,"preview result");check(counts.preview==1&&counts.real==0,"dry invoked real callback");check(!std::filesystem::exists(target),"dry wrote sentinel");
 for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"toml","{}"},{"json","{"},{"json","{\"bad key\":\"x\"}"},{"json","{\"title\":1}"}}){a={dslvm::Value::string(pair.first),dslvm::Value::string(pair.second)};bool failed=false;try{f->dry_fn(a);}catch(...){failed=true;}check(failed,"invalid format/domain accepted");check(!std::filesystem::exists(target),"invalid preview wrote sentinel");}
 a={dslvm::Value::string("json"),dslvm::Value::string("{\"title\":\"real\"}")};check(f->fn(a).b,"real result");check(counts.real==1,"real callback count");check(dslvm::read_file(target)==a[1].s,"real content");
 std::filesystem::remove(target);std::cout<<"PASS data input: preview only, format/domain validation, real once\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
