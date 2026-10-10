// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of kalman.py, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/kalman.hpp"
#include <lapacke.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace mlat {
namespace {
struct Matrix {
 int rows,cols;std::vector<double> data;
 Matrix(int r,int c):rows(r),cols(c),data(static_cast<std::size_t>(r*c),0){}
 double& operator()(int r,int c){return data[static_cast<std::size_t>(r*cols+c)];}
 double operator()(int r,int c)const{return data[static_cast<std::size_t>(r*cols+c)];}
};
constexpr int points_count=19;
double weight(int i){return i==0?-2.0:1.0/6.0;}
Matrix sigma_points(const State9& mean,const Covariance9& covariance) {
 Matrix root(9,9),points(19,9);
 for(int i=0;i<9;++i)for(int j=0;j<9;++j)root(i,j)=covariance[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
 if(LAPACKE_dpotrf(LAPACK_ROW_MAJOR,'U',9,root.data.data(),9)!=0)throw std::runtime_error("UKF covariance not positive definite");
 const double scale=std::sqrt(3.0);
 for(int j=0;j<9;++j) {
  points(0,j)=mean[static_cast<std::size_t>(j)];
  for(int i=0;i<9;++i) {
   double v=j>=i?root(i,j)*scale:0;
   points(i+1,j)=mean[static_cast<std::size_t>(j)]+v;
   points(i+10,j)=mean[static_cast<std::size_t>(j)]-v;
  }
 }
 return points;
}
std::pair<std::vector<double>,Matrix> moments(const Matrix& points,const Matrix& noise) {
 const int n=points.cols;std::vector<double> mean(static_cast<std::size_t>(n),0);Matrix cov(n,n);
 for(int j=0;j<n;++j)for(int k=0;k<19;++k)mean[static_cast<std::size_t>(j)]+=points(k,j)*weight(k);
 for(int i=0;i<n;++i)for(int j=0;j<n;++j) {
  for(int k=0;k<19;++k)cov(i,j)+=(points(k,i)-mean[static_cast<std::size_t>(i)])*weight(k)*(points(k,j)-mean[static_cast<std::size_t>(j)]);
  cov(i,j)+=noise(i,j);
 }
 return {std::move(mean),std::move(cov)};
}
Matrix inverse(Matrix a) {
 std::vector<lapack_int> piv(static_cast<std::size_t>(a.rows));
 if(LAPACKE_dgetrf(LAPACK_ROW_MAJOR,a.rows,a.cols,a.data.data(),a.cols,piv.data())!=0 ||
    LAPACKE_dgetri(LAPACK_ROW_MAJOR,a.rows,a.data.data(),a.cols,piv.data())!=0)
  throw std::runtime_error("singular UKF innovation");
 return a;
}
Matrix pseudo_inverse(Matrix a) {
 const int n=a.rows;Matrix u(n,n),vt(n,n),result(n,n);std::vector<double> s(static_cast<std::size_t>(n));
 if(LAPACKE_dgesdd(LAPACK_ROW_MAJOR,'S',n,n,a.data.data(),n,s.data(),u.data.data(),n,vt.data.data(),n)!=0)
  throw std::runtime_error("UKF SVD failed");
 double threshold=n*std::numeric_limits<double>::epsilon()*s[0];
 for(int i=0;i<n;++i)for(int j=0;j<n;++j)for(int k=0;k<n;++k)
  if(s[static_cast<std::size_t>(k)]>threshold)result(i,j)+=(vt(k,i)/s[static_cast<std::size_t>(k)])*u(j,k);
 return result;
}
}
State9 KalmanCA::transition(const State9& x,double dt)noexcept {
 State9 r=x;
 for(std::size_t i=0;i<3;++i){r[i]=x[i]+x[i+3]*dt+0.5*x[i+6]*dt*dt;r[i+3]=x[i+3]+x[i+6]*dt;}
 return r;
}
Covariance9 KalmanCA::process_covariance(double dt)noexcept {
 Covariance9 c{};
 for(std::size_t i=0;i<3;++i) {
  c[i][i]=0.25*std::pow(dt,4);c[i+3][i+3]=dt*dt;c[i+6][i+6]=1;
  c[i][i+3]=c[i+3][i]=0.5*std::pow(dt,3);
  c[i][i+6]=c[i+6][i]=0.5*dt*dt;c[i+3][i+6]=c[i+6][i+3]=dt;
 }
 for(auto& row:c)for(auto& v:row)v=v*(0.10*0.10)*dt;
 return c;
}
void KalmanCA::derive() {
 auto& s=state_;
 for(std::size_t i=0;i<3;++i){s.position[i]=s.mean[i];s.velocity[i]=s.mean[i+3];}
 double pe=s.covariance[0][0]+s.covariance[1][1]+s.covariance[2][2];
 double ve=s.covariance[3][3]+s.covariance[4][4]+s.covariance[5][5];
 s.position_error=pe<0?1e6:std::sqrt(pe);s.velocity_error=ve<0?1e6:std::sqrt(ve);
 s.llh=ecef_to_llh(s.position);double lat=s.llh[0]*constants::dtor,lon=s.llh[1]*constants::dtor;
 const auto [vx,vy,vz]=s.velocity;
 double east=-std::sin(lon)*vx+std::cos(lon)*vy;
 double north=(std::sin(-lat)*std::cos(lon))*vx+(std::sin(-lat)*std::sin(lon))*vy+std::cos(-lat)*vz;
 double up=(std::cos(-lat)*std::cos(lon))*vx+(std::cos(-lat)*std::sin(lon))*vy-std::sin(-lat)*vz;
 s.enu={east,north,up};s.heading=std::atan2(east,north)*180.0/std::numbers::pi;
 if(s.heading<0)s.heading+=360;
 s.ground_speed=std::sqrt(north*north+east*east);s.vertical_speed=up;s.derived=true;s.valid=true;
}
bool KalmanCA::update(double now,std::span<const Measurement> measurements,std::optional<double> altitude,
                      std::optional<double> altitude_error,const Vec3& ls,const Covariance3& cov,int dof) {
 // Intentional ingress bound/finite guard; malformed inputs never enter LAPACK.
 if(measurements.size()<2||measurements.size()>256)throw std::invalid_argument("UKF measurement count");
 if(!std::isfinite(now))throw std::invalid_argument("UKF nonfinite time");
 for(double v:ls)if(!std::isfinite(v))throw std::invalid_argument("UKF invalid seed");
 for(const auto& row:cov)for(double v:row)if(!std::isfinite(v))throw std::invalid_argument("UKF invalid seed covariance");
 if(altitude&&(!std::isfinite(*altitude)||!altitude_error||!std::isfinite(*altitude_error)||*altitude_error<=0))throw std::invalid_argument("UKF altitude constraint");
 for(const auto& m:measurements) {
  if(!std::isfinite(m.timestamp)||!std::isfinite(m.variance)||m.variance<=0)throw std::invalid_argument("UKF invalid measurement");
  for(double v:m.receiver)if(!std::isfinite(v))throw std::invalid_argument("UKF invalid receiver");
 }
 double dt=now-state_.last_update;
 if(dt<0)return false;
 if(dt>300)reset();
 if(state_.acquiring&&dof<1)return false;
 if(!state_.initialized) {
  state_.last_update=now;state_.initialized=true;
  for(std::size_t i=0;i<3;++i){state_.mean[i]=ls[i];for(std::size_t j=0;j<3;++j)state_.covariance[i][j]=cov[i][j]*4;}
  for(std::size_t i=3;i<6;++i)state_.covariance[i][i]=40000;
  for(std::size_t i=6;i<9;++i)state_.covariance[i][i]=1;
  return false;
 }
 if(dof<0)return false;
 try {
  int n=static_cast<int>(measurements.size())-(altitude?0:1);
  Matrix noise(n,n);std::vector<double> obs(static_cast<std::size_t>(n));
  if(altitude){obs[0]=*altitude;noise(0,0)=*altitude_error * *altitude_error;}
  double zero=measurements[0].timestamp*constants::cair;
  for(std::size_t i=1;i<measurements.size();++i){int j=static_cast<int>(i)-(altitude?0:1);
   obs[static_cast<std::size_t>(j)]=measurements[i].timestamp*constants::cair-zero;
   noise(j,j)=(measurements[i].variance+measurements[0].variance)*(constants::cair*constants::cair);
  }
  auto points=sigma_points(state_.mean,state_.covariance);
  for(int k=0;k<19;++k){State9 x;for(int j=0;j<9;++j)x[static_cast<std::size_t>(j)]=points(k,j);
   auto p=transition(x,dt);for(int j=0;j<9;++j)points(k,j)=p[static_cast<std::size_t>(j)];}
  auto process=process_covariance(dt);Matrix q(9,9);
  for(int i=0;i<9;++i)for(int j=0;j<9;++j)q(i,j)=process[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
  auto [pred,cov_pred]=moments(points,q);State9 pred9;Covariance9 cov9;
  for(int i=0;i<9;++i){pred9[static_cast<std::size_t>(i)]=pred[static_cast<std::size_t>(i)];for(int j=0;j<9;++j)cov9[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]=cov_pred(i,j);}
  auto predicted=sigma_points(pred9,cov9);Matrix obs_points(19,n);
  for(int k=0;k<19;++k) {
   Vec3 position{predicted(k,0),predicted(k,1),predicted(k,2)};
   double base=ecef_distance(measurements[0].receiver,position);
   if(altitude)obs_points(k,0)=ecef_to_llh(position)[2];
   for(std::size_t i=1;i<measurements.size();++i)obs_points(k,static_cast<int>(i)-(altitude?0:1))=ecef_distance(measurements[i].receiver,position)-base;
  }
  auto [obs_mean,obs_cov]=moments(obs_points,noise);auto inv=inverse(obs_cov);
  std::vector<double> innovation(static_cast<std::size_t>(n));
  for(int i=0;i<n;++i)innovation[static_cast<std::size_t>(i)]=obs[static_cast<std::size_t>(i)]-obs_mean[static_cast<std::size_t>(i)];
  double distance=0;
  for(int j=0;j<n;++j){double partial=0;for(int i=0;i<n;++i)partial+=innovation[static_cast<std::size_t>(i)]*inv(i,j);distance+=partial*innovation[static_cast<std::size_t>(j)];}
  if(!std::isfinite(distance)||distance<0)throw std::runtime_error("invalid UKF Mahalanobis distance");
  if(std::sqrt(distance)>15) {
   ++state_.outliers;
   if(state_.outliers<3 || now-state_.last_update<15)return false;
   reset();return false;
  }
  state_.outliers=0;
  Matrix cross(9,n);
  for(int i=0;i<9;++i)for(int j=0;j<n;++j)for(int k=0;k<19;++k)
   cross(i,j)+=(predicted(k,i)-pred[static_cast<std::size_t>(i)])*weight(k)*(obs_points(k,j)-obs_mean[static_cast<std::size_t>(j)]);
  auto pinv=pseudo_inverse(obs_cov);Matrix gain(9,n);
  for(int i=0;i<9;++i)for(int j=0;j<n;++j)for(int k=0;k<n;++k)gain(i,j)+=cross(i,k)*pinv(k,j);
  for(int i=0;i<9;++i) {
   double correction=0;for(int j=0;j<n;++j)correction+=gain(i,j)*innovation[static_cast<std::size_t>(j)];
   state_.mean[static_cast<std::size_t>(i)]=pred[static_cast<std::size_t>(i)]+correction;
   for(int j=0;j<9;++j){double delta=0;for(int k=0;k<n;++k)delta+=gain(i,k)*cross(j,k);
    state_.covariance[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]=cov_pred(i,j)-delta;}
  }
  for(double v:state_.mean)if(!std::isfinite(v))throw std::runtime_error("nonfinite UKF state");
  for(const auto& row:state_.covariance)for(double v:row)if(!std::isfinite(v))throw std::runtime_error("nonfinite UKF covariance");
  state_.last_update=now;derive();
  if(state_.acquiring&&state_.position_error<3000&&state_.velocity_error<50)state_.acquiring=false;
  else if(!state_.acquiring&&(state_.position_error>5000||state_.velocity_error>75))state_.acquiring=true;
  state_.valid=!state_.acquiring;return state_.valid;
 }catch(const std::runtime_error&){reset();return false;}
}
}
