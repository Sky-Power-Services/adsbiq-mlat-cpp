#include "mlat/csv_sink.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <unistd.h>
using namespace mlat;using nlohmann::json;namespace fs=std::filesystem;
std::size_t checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::string read(const fs::path& path){std::ifstream stream(path);return {std::istreambuf_iterator<char>(stream),{}};}
struct Directory {
 fs::path path;
 Directory(){char pattern[]="/tmp/mlat-csv-tests-XXXXXX";auto* name=mkdtemp(pattern);if(!name)throw std::runtime_error("temporary directory");path=name;}
 ~Directory(){for(unsigned i=1;i<=10;++i)fs::remove(path/("positions.csv."+std::to_string(i)));fs::remove(path/"positions.csv");fs::remove(path/"link");fs::remove(path);}
};
int main(int argc,char**argv){try{
 require(argc==2,"fixture required");json fixture;std::ifstream(argv[1])>>fixture;
 for(const auto& scenario:fixture.at("cases")){
  Directory directory;auto path=(directory.path/"positions.csv").string();
  auto make=[&](){return std::make_unique<CsvSink>(path,scenario.at("max_bytes"),scenario.at("backups"));};auto sink=make();
  for(const auto& step:scenario.at("steps")){
   if(step.at("reopen").get<bool>()){sink.reset();sink=make();}
   require(sink->write(step.at("record").get<std::string>()),"record accepted");
   json files=json::object();for(const auto& entry:fs::directory_iterator(directory.path))files[entry.path().filename().string()]=read(entry.path());
   require(files==step.at("files"),"exact Python active/backups content after each record");
   require(sink->errors()==0,"no silent errors");
  }
 }
 {Directory directory;auto path=directory.path/"positions.csv";CsvSink sink(path.string(),10,1);
  require(sink.write("first"),"failure case initial record");
  auto backup=directory.path/"positions.csv.1";fs::create_directory(backup);std::ofstream(backup/"obstacle")<<"keep";
  require(!sink.write("second")&&sink.errors()==1,"rotation failure is contained and counted");
  require(read(backup/"obstacle")=="keep"&&read(path)=="first\n","failed rollover preserves original data");
  fs::remove(backup/"obstacle");fs::remove(backup);
  require(sink.write("second")&&sink.errors()==1,"retry reopens after failed rollover");
  require(read(path)=="second\n"&&read(backup)=="first\n","recovered rotation has exact data");
  require(!sink.write(std::string(1024*1024+1,'x'))&&sink.errors()==2,"oversized record bounded");
 }
 {Directory directory;auto path=directory.path/"positions.csv";std::ofstream(path)<<"untouched";fs::create_symlink(path,directory.path/"link");bool rejected=false;
  try{CsvSink bad((directory.path/"link").string());}catch(const std::exception&){rejected=true;}
  require(rejected&&read(path)=="untouched","symlink destination rejected without modifying target");
  rejected=false;try{CsvSink bad(directory.path.string());}catch(const std::exception&){rejected=true;}require(rejected,"nonregular destination rejected");
 }
 {Directory directory;auto path=directory.path/"positions.csv";
  {std::ofstream(path)<<"private-existing";}fs::permissions(path,fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read);
  bool rejected=false;try{CsvSink unsafe(path.string(),1024*1024,2,true);}catch(const std::exception&){rejected=true;}
  require(rejected&&read(path)=="private-existing","private sink rejects exposed existing log without modifying it");
  fs::permissions(path,fs::perms::owner_read|fs::perms::owner_write);fs::remove(path);
  const std::string record="{\"event\":\"handshake\",\"redacted\":true}";
  {CsvSink sink(path.string(),1024*1024,2,true);for(int i=0;i<90000;++i)require(sink.write(record),"bounded handshake journal write");}
  require(fs::exists(path.string()+".2")&&!fs::exists(path.string()+".3"),"handshake journal retains exactly two backups");
  for(int i=0;i<=2;++i){auto file=path.string()+(i?"."+std::to_string(i):"");
   require(fs::file_size(file)<1024*1024,"handshake journal one MiB rollover bound");
   require((fs::status(file).permissions()&fs::perms::all)==(fs::perms::owner_read|fs::perms::owner_write),"handshake journal and backups remain mode0600");
   std::ifstream input(file);std::string line;while(std::getline(input,line))require(line==record,"only allowlisted fixed event persisted");
  }
  fs::permissions(path.string()+".2",fs::perms::owner_read|fs::perms::owner_write|fs::perms::others_read);
  rejected=false;try{CsvSink unsafe(path.string(),1024*1024,2,true);}catch(const std::exception&){rejected=true;}
  require(rejected,"private sink also rejects exposed historical backup");
 }
 std::cout<<"PASS "<<checks<<" CSV sink checks against 32 Python rotation steps and failure cases\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
