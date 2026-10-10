#include "mlat/kalman.hpp"
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
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream in(argv[1]);json fixture;in>>fixture;
  std::size_t checks=0,valid=0,resets=0,losses=0;double max_mean=0,max_cov=0;
  for(const auto& c:fixture.at("cases")) {
   KalmanCA filter;bool was_initialized=false,was_valid=false;
   for(std::size_t index=0;index<c.at("steps").size();++index) {
    auto label=c.at("name").get<std::string>()+" step "+std::to_string(index);
    const auto& step=c.at("steps")[index];const auto& e=c.at("expected")[index];
    std::vector<Measurement> ms;
    for(const auto& m:step.at("measurements"))ms.push_back({m.at("receiver").get<Vec3>(),m.at("timestamp"),m.at("variance")});
    std::optional<double> alt,err;if(!step.at("altitude").is_null())alt=step.at("altitude");if(!step.at("altitude_error").is_null())err=step.at("altitude_error");
    bool accepted=filter.update(step.at("time"),ms,alt,err,step.at("position").get<Vec3>(),step.at("covariance").get<Covariance3>(),step.at("dof"));
    const auto& s=filter.state();
    auto require=[&](bool ok,const std::string& field){++checks;if(!ok)throw std::runtime_error(label+" "+field);};
    auto near=[&](double a,double b,double tolerance,const std::string& field){require(std::isfinite(a)&&std::abs(a-b)<=tolerance,field+" expected="+std::to_string(b)+" actual="+std::to_string(a));};
    require(accepted==e.at("accepted"),"return value");require(s.valid==e.at("valid"),"valid");require(s.acquiring==e.at("acquiring"),"acquiring");
    require(s.outliers==e.at("outliers"),"outlier count");require(s.last_update==e.at("last_update"),"last_update");
    require(s.initialized!=e.at("mean").is_null(),"initialized");require(s.derived!=e.at("position").is_null(),"derived");
    valid+=s.valid;resets+=was_initialized&&!s.initialized;losses+=was_valid&&!s.valid;was_initialized=s.initialized;was_valid=s.valid;
    if(s.initialized) {
     for(std::size_t i=0;i<9;++i){double expected=e.at("mean")[i];max_mean=std::max(max_mean,std::abs(s.mean[i]-expected));near(s.mean[i],expected,0.001,"mean "+std::to_string(i));}
     double squared=0,norm=0;
     for(std::size_t i=0;i<9;++i)for(std::size_t j=0;j<9;++j) {
      double expected=e.at("covariance")[i][j];double d=s.covariance[i][j]-expected;squared+=d*d;norm+=expected*expected;
      near(s.covariance[i][j],expected,1e-4+1e-5*std::abs(expected),"covariance");
     }
     max_cov=std::max(max_cov,std::sqrt(squared/std::max(norm,1e-30)));
    }
    if(s.derived) {
     for(const auto& [name,value]:std::vector<std::pair<std::string,Vec3>>{{"position",s.position},{"velocity",s.velocity},{"llh",s.llh},{"enu",s.enu}})
      for(std::size_t i=0;i<3;++i)near(value[i],e.at(name)[i],name=="llh"&&i<2?1e-7:0.001,name);
     for(const auto& [name,value]:std::vector<std::pair<std::string,double>>{{"position_error",s.position_error},{"velocity_error",s.velocity_error},{"heading",s.heading},{"ground_speed",s.ground_speed},{"vertical_speed",s.vertical_speed}})
      near(value,e.at(name),0.001,name);
    }
   }
  }
  if(!valid||!resets||!losses)throw std::runtime_error("insufficient lifecycle coverage");
  KalmanCA guarded;std::vector<Measurement> ms(2,{{1,2,3},1000,1e-14});Covariance3 cov{};
  for(std::size_t i=0;i<3;++i)cov[i][i]=10000;
  auto reject=[&](double time,std::optional<double> alt,std::optional<double> err) {
   bool threw=false;try{guarded.update(time,ms,alt,err,{1,2,3},cov,2);}catch(const std::invalid_argument&){threw=true;}
   ++checks;if(!threw||guarded.state().initialized)throw std::runtime_error("invalid UKF input changed state");
  };
  reject(std::numeric_limits<double>::quiet_NaN(),{},{});reject(1000,1000,{});reject(1000,1000,0);
  ms[0].variance=0;reject(1000,{},{});ms[0].variance=1e-14;
  ms.resize(257);reject(1000,{},{});ms.clear();reject(1000,{},{});
  std::cout<<checks<<" UKF assertions; valid states="<<valid<<" resets="<<resets<<" valid-to-invalid="<<losses
           <<" max_mean_abs_error="<<max_mean<<" max_relative_covariance_error="<<max_cov<<'\n';
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
