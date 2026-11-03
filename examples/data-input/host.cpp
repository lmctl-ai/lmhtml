// Standalone sample, not an extension of lmhtml's product schema or SDK.
#include "dslvm.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
namespace data_input {
const std::string schema=R"({"dsl.schema":2,"name":"data_input_example","functions":{"accept_attributes":[{"params":["string","string"],"returns":"bool"}]}})";
struct Counts {int real=0,preview=0;};
static void validate(dslvm::Registry& registry,dslvm::Args& args){
 if(args[0].s!="json")throw dslvm::Error("accept_attributes: expected format json");
 dslvm::Args input{args[1]};const auto value=registry.function_by_signature("fn json_parse(string) -> Json")->fn(input);
 const auto& node=*static_cast<dslvm::stdlib::JsonNode*>(value.obj);
 if(node.kind!=dslvm::stdlib::JsonNode::Kind::Object)throw dslvm::Error("accept_attributes: expected an attribute object");
 for(const auto& member:node.object){
  const auto valid=[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c==':'||c=='.';};
  if(member.key.empty()||!std::all_of(member.key.begin(),member.key.end(),valid))throw dslvm::Error("accept_attributes: invalid attribute name");
  if(member.value->kind!=dslvm::stdlib::JsonNode::Kind::String)throw dslvm::Error("accept_attributes: attribute values must be strings");
 }
}
void register_input(dslvm::Registry& registry,Counts& counts,const std::string& target){
 dslvm::Function real=[&registry,&counts,target](dslvm::Args&a){++counts.real;validate(registry,a);std::ofstream out(target,std::ios::binary|std::ios::trunc);out<<a[1].s;if(!out)throw dslvm::Error("accept_attributes: cannot write target");return dslvm::Value::boolean(true);};
 registry.function("accept_attributes",{"string","string"},"bool",real,dslvm::dry(dslvm::Function([&registry,&counts](dslvm::Args&a){++counts.preview;validate(registry,a);return dslvm::Value::boolean(true);})));
}
}
int main(int argc,char**argv){
 if(argc>=2&&std::string(argv[1])=="--lmcode-schema"){std::cout<<data_input::schema;return 0;}
 if(argc<4||std::string(argv[2])!="--lmcode")return dslvm::failure_frame("cli",dslvm::Error("CLI_USAGE","expected TARGET --lmcode FILE..."));
 std::vector<std::string> files;for(int i=3;i<argc;i++)files.emplace_back(argv[i]);
 try{
  auto results=dslvm::InvocationResults::from_command(files);dslvm::Registry registry;data_input::Counts counts;data_input::register_input(registry,counts,argv[1]);dslvm::Context context;context.set_dry(results.dry());
  for(const auto& part:results.registrations()){
   try{auto program=dslvm::Program::load(dslvm::read_file(part.file),data_input::schema,registry);results.select(part.index,registry,dslvm::ResultContract::from_schema(data_input::schema));program.run(context,registry);results.complete(part.index);}
   catch(const dslvm::Error&e){results.fail(part.index,"run",e);break;}
  }
  std::cerr<<"preview="<<counts.preview<<" real="<<counts.real<<"\n";return results.write_frame(std::cout);
 }catch(const dslvm::Error&e){return dslvm::failure_frame("cli",e);}
}
