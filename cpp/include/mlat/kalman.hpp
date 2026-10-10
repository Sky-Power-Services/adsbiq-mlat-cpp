// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of kalman.py, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/solver.hpp"
namespace mlat {
using State9=std::array<double,9>;
using Covariance9=std::array<std::array<double,9>,9>;
struct FilterState {
 bool initialized=false,derived=false,acquiring=true,valid=false;
 int outliers=0;double last_update=0;
 State9 mean{};Covariance9 covariance{};
 Vec3 position{},velocity{},llh{},enu{};
 double position_error=0,velocity_error=0,heading=0,ground_speed=0,vertical_speed=0;
};
class KalmanCA {
public:
 bool update(double position_time,std::span<const Measurement> measurements,
             std::optional<double> altitude,std::optional<double> altitude_error,
             const Vec3& leastsquares_position,const Covariance3& leastsquares_cov,int dof);
 const FilterState& state()const noexcept{return state_;}
 void reset()noexcept {state_=FilterState{};}
 static State9 transition(const State9& state,double dt)noexcept;
 static Covariance9 process_covariance(double dt)noexcept;
private:
 void derive();
 FilterState state_{};
};
}
