#include "mlat/solver.hpp"
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
  if(argc!=2)throw std::runtime_error("fixture required");
  std::ifstream in(argv[1]);json fixture;in>>fixture;
  std::size_t checks=0,failures=0,accepted=0,rejected=0,no_covariance=0;
  double max_position=0,max_covariance=0,max_degenerate_position=0;
  std::size_t degenerate_rejections=0;
  std::size_t evaluation_limit_rejections=0;
  for(const auto& c:fixture.at("cases")) {
   const auto name=c.at("name").get<std::string>();
   std::vector<Measurement> measurements;
   for(const auto& m:c.at("measurements"))measurements.push_back({m.at("receiver").get<Vec3>(),m.at("timestamp"),m.at("variance")});
   std::optional<double> altitude,error;
   if(!c.at("altitude").is_null())altitude=c.at("altitude");
   if(!c.at("altitude_error").is_null())error=c.at("altitude_error");
   std::optional<Solution> actual;SolverDiagnostics diag;bool threw=false;
   try{actual=solve(measurements,altitude,error,c.at("guess").get<Vec3>(),&diag);}catch(const std::invalid_argument&){threw=true;}
   auto require=[&](bool ok,const std::string& reason){++checks;if(!ok){++failures;std::cerr<<name<<": "<<reason<<" (status "<<diag.status<<", evaluations "<<diag.evaluations<<")\n";}};
   require(threw==c.contains("exception"),"exception parity");
   if(threw||c.contains("exception"))continue;
   require(diag.evaluations>0&&diag.evaluations<=169,"MINPACK evaluation budget");
   evaluation_limit_rejections+=diag.status==5;
   const auto& expected=c.at("expected");
   require(actual.has_value()!=expected.is_null(),"acceptance parity");
   if(!actual||expected.is_null()){++rejected;continue;}
   ++accepted;
   Solution baseline{expected.at("position").get<Vec3>(),{}};
   if(!expected.at("covariance").is_null())baseline.covariance=expected.at("covariance").get<Covariance3>();
   auto actual_error=tracker_position_error(*actual),expected_error=tracker_position_error(baseline);
   bool usable=actual_error&&*actual_error<=10000,expected_usable=expected_error&&*expected_error<=10000;
   require(usable==expected_usable,"tracker covariance gate parity");
   double position_error=ecef_distance(actual->position,expected.at("position").get<Vec3>());
   // Identical receiver positions cannot identify a unique transmitter position.
   // Do NOT require a particular arbitrary LM solution in this rank-deficient
   // case. Require both implementations to reject it at the actual tracker gate
   // and verify C++ solved the same equations. All other cases retain strict
   // 10 cm / 0.1% covariance comparisons, including near-collinear geometry.
   bool coincident=std::all_of(measurements.begin(),measurements.end(),[&](const auto& m){return m.receiver==measurements.front().receiver;});
   if(coincident) {
    ++degenerate_rejections;max_degenerate_position=std::max(max_degenerate_position,position_error);
    require(!usable&&!expected_usable,"degenerate geometry must not reach output");
    require(actual->covariance.has_value()!=expected.at("covariance").is_null(),"degenerate covariance presence parity");
    no_covariance+=!actual->covariance;
    double range_residual=std::abs(ecef_distance(measurements[0].receiver,actual->position)-diag.offset);
    require(range_residual<=0.01,"degenerate range residual");
    if(altitude)require(std::abs(ecef_to_llh(actual->position)[2]-*altitude)<=0.01,"degenerate altitude residual");
    continue;
   }
   max_position=std::max(max_position,position_error);
   require(position_error<=0.1,"position error "+std::to_string(position_error)+" m");
   require(actual->covariance.has_value()!=expected.at("covariance").is_null(),"covariance presence parity");
   if(!actual->covariance){++no_covariance;continue;}
   if(expected.at("covariance").is_null())continue;
   const auto e=expected.at("covariance").get<Covariance3>();double delta=0,norm=0;
   for(std::size_t i=0;i<3;++i)for(std::size_t j=0;j<3;++j) {
    const double d=(*actual->covariance)[i][j]-e[i][j];delta+=d*d;norm+=e[i][j]*e[i][j];
   }
   double cov_error=std::sqrt(delta/std::max(norm,1e-30));max_covariance=std::max(max_covariance,cov_error);
   require(std::isfinite(cov_error)&&cov_error<=0.001,"relative covariance error "+std::to_string(cov_error));
  }
  std::vector<Measurement> bad(4,{{1,2,3},1000,1e-14});
  for(double invalid:{0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
   bad[0].variance=invalid;bool threw=false;try{solve(bad,{}, {},{1,2,3});}catch(const std::invalid_argument&){threw=true;}
   ++checks;if(!threw)throw std::runtime_error("invalid variance accepted");
  }
  bad[0].variance=1e-14;bad[0].timestamp=1001;
  bool threw=false;try{solve(bad,{}, {},{1,2,3});}catch(const std::invalid_argument&){threw=true;}
  ++checks;if(!threw)throw std::runtime_error("unordered timestamps accepted");
  Solution gate{{},Covariance3{}};
  (*gate.covariance)[0][0]=10000.9*10000.9;
  ++checks;if(tracker_position_error(gate)!=10000)throw std::runtime_error("tracker floor boundary changed");
  (*gate.covariance)[0][0]=10001.0*10001.0;
  ++checks;if(tracker_position_error(gate)!=10001)throw std::runtime_error("tracker rejection boundary changed");
  gate.covariance.reset();++checks;if(tracker_position_error(gate))throw std::runtime_error("missing covariance accepted");
  bad[0].timestamp=1000;
  auto invalid_call=[&](std::optional<double> alt,std::optional<double> err,Vec3 guess) {
   bool rejected=false;try{solve(bad,alt,err,guess);}catch(const std::invalid_argument&){rejected=true;}
   ++checks;if(!rejected)throw std::runtime_error("invalid solver input accepted");
  };
  invalid_call(1000,{}, {1,2,3});invalid_call(1000,0,{1,2,3});
  invalid_call({}, {}, {std::numeric_limits<double>::quiet_NaN(),2,3});
  bad.resize(4097);threw=false;
  try{solve(bad,{}, {}, {1,2,3});}catch(const std::length_error&){threw=true;}
  ++checks;if(!threw)throw std::runtime_error("measurement bound ignored");
  std::cout<<checks<<" solver checks; accepted="<<accepted<<" rejected="<<rejected<<" missing_covariance="<<no_covariance
           <<" max_position_error_m="<<max_position<<" max_relative_covariance_error="<<max_covariance
           <<" degenerate_gate_rejections="<<degenerate_rejections<<" degenerate_raw_position_difference_m="<<max_degenerate_position
           <<" evaluation_limit_rejections="<<evaluation_limit_rejections<<" failures="<<failures<<'\n';
  return failures?1:0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
