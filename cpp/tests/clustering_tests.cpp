#include "mlat/mlat_tracker.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <stdexcept>
using nlohmann::json;
using namespace mlat;
int main(int argc,char**argv) {
 try {
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream in(argv[1]);json data;in>>data;std::size_t checks=0,count=0;
  for(const auto& c:data.at("cases")) {
   std::map<clock::StationId,std::shared_ptr<ReceiverModel>> rs;
   for(const auto& r:c.at("receivers")){auto rx=std::make_shared<ReceiverModel>(r.at("id"),"beast",Vec3{0,0,0},0);rx->position=r.at("position").get<Vec3>();rs.emplace(rx->id,rx);}
   std::vector<clock::NormalizedStation> component;
   for(const auto& r:c.at("component")){clock::NormalizedStation s{r.at("id"),r.at("variance"),{}};for(const auto& t:r.at("samples"))s.samples.push_back({t[0],t[1]});component.push_back(s);}
   auto clusters=cluster_timestamps(component,rs,c.at("minimum"));const auto& expected=c.at("expected");
   auto require=[&](bool ok){++checks;if(!ok)throw std::runtime_error(c.at("name").get<std::string>()+" clustering mismatch");};
   require(clusters.size()==expected.size());count+=clusters.size();
   for(std::size_t i=0;i<clusters.size();++i){const auto& a=clusters[i];const auto& e=expected[i];require(a.distinct==e.at("distinct"));require(a.first_seen==e.at("first_seen"));require(a.samples.size()==e.at("samples").size());
    for(std::size_t j=0;j<a.samples.size();++j){require(a.samples[j].receiver->id==e.at("samples")[j][0]);require(a.samples[j].timestamp==e.at("samples")[j][1]);require(a.samples[j].variance==e.at("samples")[j][2]);}
   }
  }
  ++checks;if(!cluster_timestamps({}, {},3).empty())throw std::runtime_error("empty cluster guard");
  bool threw=false;try{cluster_timestamps({}, {},0);}catch(const std::invalid_argument&){threw=true;}++checks;if(!threw)throw std::runtime_error("minimum guard");
  threw=false;try{cluster_timestamps({{1,1,{{std::numeric_limits<double>::quiet_NaN(),0}}}}, {},3);}catch(const std::invalid_argument&){threw=true;}++checks;if(!threw)throw std::runtime_error("nonfinite guard");
  std::cout<<checks<<" clustering assertions; "<<count<<" clusters across "<<data.at("cases").size()<<" scenarios\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
