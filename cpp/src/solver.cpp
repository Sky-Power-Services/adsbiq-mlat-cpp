// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of solver.py, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/solver.hpp"
#include <cminpack.h>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace mlat {
namespace {
struct Range {Vec3 position;double distance,error;};
struct Context {std::vector<Range> ranges;std::optional<double> altitude;double altitude_error;};
int residuals(void* opaque,int,int,const double* x,double* out,int) noexcept {
 const auto& c=*static_cast<const Context*>(opaque);
 const Vec3 position{x[0],x[1],x[2]};
 std::size_t i=0;
 for(const auto& r:c.ranges) {
  out[i]=(r.distance-(ecef_distance(r.position,position)-x[3]))/r.error;
  if(!std::isfinite(out[i]))return -1;
  ++i;
 }
 if(c.altitude) {
  out[i]=(*c.altitude-ecef_to_llh(position)[2])/c.altitude_error;
  if(!std::isfinite(out[i]))return -1;
 }
 return 0;
}
// SciPy leastsq uses an exact triangular inverse, not a tolerance-based
// pseudoinverse; do not substitute cminpack covar's rank truncation.
std::optional<Covariance3> covariance(const std::vector<double>& qr,int m,const std::array<int,4>& pivot) {
 double inverse[4][4]{},permuted[4][4]{};
 for(int column=0;column<4;++column) {
  for(int row=3;row>=0;--row) {
   const double diagonal=qr[static_cast<std::size_t>(row+row*m)];
   if(diagonal==0)return std::nullopt;
   double rhs=row==column?1.0:0.0;
   for(int k=row+1;k<4;++k) rhs-=qr[static_cast<std::size_t>(row+k*m)]*inverse[k][column];
   inverse[row][column]=rhs/diagonal;
  }
 }
 for(int row=0;row<4;++row) for(int j=0;j<4;++j) permuted[pivot[static_cast<std::size_t>(row)]-1][j]=inverse[row][j];
 Covariance3 result{};
 for(int i=0;i<3;++i) for(int j=0;j<3;++j)
  for(int k=0;k<4;++k) result[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]+=permuted[i][k]*permuted[j][k];
 return result;
}
bool finite(const Vec3& v) {return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
}
std::optional<double> tracker_position_error(const Solution& solution) noexcept {
 if(!solution.covariance)return std::nullopt;
 const auto& c=*solution.covariance;
 for(const auto& row:c) for(double value:row) if(!std::isfinite(value))return std::nullopt;
 const double trace=c[0][0]+c[1][1]+c[2][2];
 if(!std::isfinite(trace))return std::nullopt;
 return std::floor(std::sqrt(std::abs(trace)));
}
std::optional<Solution> solve(std::span<const Measurement> measurements,std::optional<double> altitude,
                             std::optional<double> altitude_error,Vec3 guess,SolverDiagnostics* diagnostics) {
 if(diagnostics)*diagnostics={};
 if(measurements.size()>4096)throw std::length_error("solver measurement bound");
 if(measurements.size()+(altitude?1U:0U)<4)throw std::invalid_argument("not enough measurements");
 if(!finite(guess))throw std::invalid_argument("invalid initial guess");
 if(altitude && (!std::isfinite(*altitude)||!altitude_error||!std::isfinite(*altitude_error)||*altitude_error<=0))
  throw std::invalid_argument("invalid altitude constraint");
 Context context{{},altitude,altitude_error.value_or(1)};
 context.ranges.reserve(measurements.size());
 const double base=measurements.front().timestamp;
 double previous=base;
 for(const auto& m:measurements) {
  if(!finite(m.receiver)||!std::isfinite(m.timestamp)||m.timestamp<previous||!std::isfinite(m.variance)||m.variance<=0)
   throw std::invalid_argument("invalid or unordered measurement");
  previous=m.timestamp;
  double distance=(m.timestamp-base)*constants::cair,error=std::sqrt(m.variance)*constants::cair;
  if(!std::isfinite(distance)||!std::isfinite(error)||error<=0)throw std::invalid_argument("invalid pseudorange");
  context.ranges.push_back({m.receiver,distance,error});
 }
 auto llh=ecef_to_llh(guess);
 constexpr double min_alt=-1500*constants::ftom,max_alt=75000*constants::ftom;
 if(llh[2]<min_alt){llh[2]=min_alt;guess=llh_to_ecef(llh);}
 if(llh[2]>max_alt){llh[2]=max_alt;guess=llh_to_ecef(llh);}
 std::array<double,4> x{guess[0],guess[1],guess[2],0},diag{},qtf{},wa1{},wa2{},wa3{};
 std::array<int,4> pivot{};
 int m=static_cast<int>(measurements.size()+(altitude?1U:0U)),evaluations=0;
 std::vector<double> fvec(static_cast<std::size_t>(m)),qr(static_cast<std::size_t>(4*m)),wa4(static_cast<std::size_t>(m));
 int status=lmdif(residuals,&context,m,4,x.data(),fvec.data(),1.49012e-8,1.49012e-8,0,165,
     std::numeric_limits<double>::epsilon(),diag.data(),1,100,0,&evaluations,qr.data(),m,pivot.data(),
     qtf.data(),wa1.data(),wa2.data(),wa3.data(),wa4.data());
 if(diagnostics)*diagnostics={status,evaluations,x[3]};
 if(status<1||status>4||!std::isfinite(x[3])||x[3]<0||x[3]>500e3)return std::nullopt;
 Vec3 position{x[0],x[1],x[2]};
 if(!finite(position))return std::nullopt;
 return Solution{position,covariance(qr,m,pivot)};
}
}
