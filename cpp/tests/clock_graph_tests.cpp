#include "mlat/clock_graph.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
using nlohmann::json;
using namespace mlat::clock;
int main(int argc,char**argv) {
 try {
  if(argc!=2)throw std::runtime_error("fixture required");
  std::ifstream in(argv[1]);json fixture;in>>fixture;
  std::size_t checks=0;
  auto near=[&](double a,double e,const std::string& context) {
   ++checks;
   if(!std::isfinite(a)||std::abs(a-e)>std::max(1e-25,std::abs(e)*2e-14))
    throw std::runtime_error(context+" expected "+std::to_string(e)+" actual "+std::to_string(a));
  };
  for(const auto& c:fixture.at("cases")) {
   auto name=c.at("name").get<std::string>();
   std::vector<StationSamples> stations;
   for(const auto& d:c.at("stations")) {
    StationSamples s{d.at("id"),{d.at("frequency"),100e-6,d.at("jitter")},{}};
    for(const auto& ts:d.at("samples")) s.samples.push_back({ts[0],ts[1]});
    stations.push_back(std::move(s));
   }
   std::vector<PairModel> pairs;
   for(const auto& d:c.at("pairs")) {
    PairModel p{d.at("base"),d.at("peer"),{}};
#define FIELD(x) p.state.x=d.at(#x)
    FIELD(valid);FIELD(variance);FIELD(updated);FIELD(n);FIELD(base_avg);FIELD(peer_avg);FIELD(factor);FIELD(i_factor);
#undef FIELD
    pairs.push_back(p);
   }
   auto actual=normalize(stations,pairs,fixture.at("metadata").at("now"));
   const auto& components=c.at("expected");++checks;
   if(components.empty()) {if(!actual.empty())throw std::runtime_error(name+" expected empty");continue;}
   const auto& expected=components[0];
   if(actual.size()!=expected.size())throw std::runtime_error(name+" component size mismatch");
   for(std::size_t i=0;i<actual.size();++i) {
    const auto& a=actual[i];const auto& e=expected[i];checks+=2;
    if(a.id!=e.at("id").get<StationId>())throw std::runtime_error(name+" traversal mismatch at "+std::to_string(i)+" expected "+e.at("id").dump()+" actual "+std::to_string(a.id));
    if(a.samples.size()!=e.at("samples").size())throw std::runtime_error(name+" sample count mismatch");
    near(a.variance,e.at("variance"),name+" variance id "+std::to_string(a.id));
    for(std::size_t j=0;j<a.samples.size();++j) {
     near(a.samples[j].ticks,e.at("samples")[j][0],name+" seconds");
     ++checks;if(a.samples[j].utc!=e.at("samples")[j][1].get<double>())throw std::runtime_error(name+" UTC altered");
    }
   }
  }
  // Defined bounds at the integration boundary, beyond Python parity.
  std::vector<StationSamples> invalid{{1,make_clock("dump1090"),{}},{1,make_clock("dump1090"),{}}};
  bool threw=false;try{normalize(invalid,{},0);}catch(const std::invalid_argument&){threw=true;}
  if(!threw)throw std::runtime_error("duplicate receiver accepted");++checks;
  std::vector<StationSamples> chain;
  std::vector<PairModel> links;
  for(StationId i=0;i<1024;++i) {
   chain.push_back({i,make_clock("dump1090"),{{12e9,1700000000}}});
   if(i) {State s;s.valid=true;s.n=12;s.factor=s.i_factor=1;s.variance=1e-12;s.updated=1000000;
    links.push_back({i-1,i,s});}
  }
  auto long_result=normalize(chain,links,1000000);
  if(long_result.size()!=1024)throw std::runtime_error("long chain truncated");++checks;
  for(const auto& station:long_result) near(station.samples[0].ticks,1000,"long chain seconds");
  auto must_reject=[&](const std::string& name) {
   bool rejected=false;try{normalize(chain,links,1000000);}catch(const std::invalid_argument&){rejected=true;}
   if(!rejected)throw std::runtime_error(name);++checks;
  };
  links[0].state.n=-2147483647;must_reject("negative sample count accepted");links[0].state.n=12;
  links[0].state.factor=0;must_reject("zero conversion factor accepted");links[0].state.factor=1;
  links.push_back(links[0]);must_reject("duplicate pair accepted");links.pop_back();
  chain[0].samples[0].ticks=std::numeric_limits<double>::infinity();must_reject("infinite ticks accepted");
  chain[0].samples[0].ticks=12e9;
  chain.resize(4097);
  threw=false;try{normalize(chain,links,1000000);}catch(const std::length_error&){threw=true;}
  if(!threw)throw std::runtime_error("graph limit ignored");++checks;
  std::cout<<checks<<" normalization assertions passed across "<<fixture.at("cases").size()<<" scenarios\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
