// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of mlattrack.py, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/mlat_tracker.hpp"
#include "mlat/modes.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace mlat {
std::vector<Cluster> cluster_timestamps(const std::vector<clock::NormalizedStation>& component,
 const std::map<clock::StationId,std::shared_ptr<ReceiverModel>>& receivers,int minimum) {
 if(minimum<1)throw std::invalid_argument("invalid cluster minimum");
 struct Entry{std::shared_ptr<ReceiverModel> receiver;double timestamp,variance,utc;};
 std::vector<Entry> flat;
 for(const auto& s:component)for(const auto& t:s.samples) {
  if(!std::isfinite(s.variance)||s.variance<0||!std::isfinite(t.ticks)||!std::isfinite(t.utc))throw std::invalid_argument("invalid cluster observation");
  if(flat.size()>=65536)throw std::length_error("cluster sample bound");
  flat.push_back({receivers.at(s.id),t.ticks,s.variance,t.utc});
 }
 if(flat.empty())return {};
 std::stable_sort(flat.begin(),flat.end(),[](const auto& a,const auto& b){return a.timestamp<b.timestamp;});
 std::vector<std::vector<Entry>> groups(1);
 for(auto& t:flat){if(!groups.back().empty()&&t.timestamp-groups.back().back().timestamp>2e-3)groups.emplace_back();groups.back().push_back(std::move(t));}
 std::vector<Cluster> clusters;
 for(auto& group:groups)while(group.size()>=static_cast<std::size_t>(minimum)) {
  auto head=group.back();group.pop_back();Cluster c{1,head.utc,{{head.receiver,head.timestamp,head.variance}}};
  for(std::size_t i=group.size();i-->0;) {
   const auto& t=group[i];if(head.timestamp-t.timestamp>2e-3)break;
   bool distinct=true,can=true;
   for(const auto& other:c.samples) {
    if(t.receiver==other.receiver){can=false;break;}
    double d=ecef_distance(t.receiver->position,other.receiver->position);
    if(std::abs(other.timestamp-t.timestamp)>(d*1.05+1e3)/constants::cair){can=false;break;}
    if(d<1e3)distinct=false;
   }
   if(can){c.samples.push_back({t.receiver,t.timestamp,t.variance});c.first_seen=std::min(c.first_seen,t.utc);if(distinct)++c.distinct;group.erase(group.begin()+static_cast<std::ptrdiff_t>(i));}
  }
  if(c.distinct>=minimum){std::reverse(c.samples.begin(),c.samples.end());clusters.push_back(std::move(c));}
 }
 return clusters;
}
MlatTracker::MlatTracker(AircraftLookup lookup,PairLookup pairs,Output output,double wall,double mono,TimerId timer_id):
 lookup_(std::move(lookup)),pairs_(std::move(pairs)),output_(std::move(output)),current_(std::make_shared<Cohort>(Cohort{wall,mono+.9,{}})),timer_id_(std::move(timer_id)){
 if(!std::isfinite(wall)||!std::isfinite(mono))throw std::invalid_argument("invalid MLAT start time");
 current_->timer_id=timer_id_?timer_id_():++local_timer_;cohorts_.push_back(current_);
}
void MlatTracker::receive(std::shared_ptr<ReceiverModel> receiver,double timestamp,std::span<const std::uint8_t> msg,double wall,double mono,bool decoded) {
 if(!receiver||!std::isfinite(timestamp)||!std::isfinite(wall)||!std::isfinite(mono)||(msg.size()!=7&&msg.size()!=14))throw std::invalid_argument("invalid MLAT observation");
 ++stats_.messages;Key key{std::string(reinterpret_cast<const char*>(msg.data()),msg.size()),decoded?++unique_:0};
 auto it=pending_.find(key);
 if(it==pending_.end()) {
  if(pending_.size()>=65536){++stats_.bounded_drops;return;}
  auto group=std::make_shared<Group>(Group{key,wall,{},{}});it=pending_.emplace(key,group).first;
  if(wall-current_->created>.05||current_->groups.size()>25){current_=std::make_shared<Cohort>(Cohort{wall,mono+.9,{},timer_id_?timer_id_():++local_timer_});cohorts_.push_back(current_);}
  current_->groups.push_back(group);
 }
 auto& group=*it->second;
 if(!group.receivers.contains(receiver->id)) {
  if(pending_receivers_>=1000000){++stats_.bounded_drops;return;}
  group.receivers.emplace(receiver->id,receiver);++pending_receivers_;
 }
 if(group.copies.size()>15)return;
 group.copies.push_back({std::move(receiver),timestamp,wall});
}
void MlatTracker::advance(double mono,double wall) {
 if(!std::isfinite(mono)||!std::isfinite(wall))throw std::invalid_argument("invalid MLAT timer time");
 while(advance_one(mono,wall)){}
}
bool MlatTracker::advance_one(double mono,double wall){
 if(!std::isfinite(mono)||!std::isfinite(wall))throw std::invalid_argument("invalid MLAT timer time");
 if(cohorts_.empty()||cohorts_.front()->due>mono)return false;
 auto cohort=cohorts_.front();cohorts_.pop_front();
 for(auto& group:cohort->groups){pending_.erase(group->key);pending_receivers_-=group->receivers.size();resolve(*group,wall);}
 return true;
}
std::optional<double> MlatTracker::next_deadline()const noexcept{return cohorts_.empty()?std::nullopt:std::optional(cohorts_.front()->due);}
std::vector<std::pair<double,std::uint64_t>> MlatTracker::ready_timers(double mono)const {
 std::vector<std::pair<double,std::uint64_t>> result;
 for(const auto& c:cohorts_){if(c->due>mono)break;result.emplace_back(c->due,c->timer_id);}return result;
}
void MlatTracker::resolve(Group& group,double now) {
 if(group.copies.size()<3)return;
 // Unique decoded-object groups normally have one copy. Never reinterpret them
 // as raw-byte groups if a caller replays such an event incorrectly.
 if(group.key.unique)return;
 std::optional<modes::Message> decoded;
 try{decoded=modes::decode(std::span(reinterpret_cast<const std::uint8_t*>(group.key.bytes.data()),group.key.bytes.size()));}catch(const std::invalid_argument&){return;}
 if(!decoded||!decoded->address)return;
 auto ac=lookup_(decoded->address);if(!ac)return;
 ac->seen=now;++ac->mlat_message_count;++stats_.valid_groups;if(!ac->allow_mlat)return;
 if(decoded->altitude&&*decoded->altitude>-1500&&*decoded->altitude<75000) {
  if(!ac->last_altitude_time||(group.first_seen>ac->last_altitude_time&&(group.first_seen-ac->last_altitude_time>15||std::abs(ac->altitude.value_or(0)-*decoded->altitude)<4000))) {
   ac->altitude=decoded->altitude;ac->last_altitude_time=group.first_seen;
   std::erase_if(ac->alt_history,[&](const auto& entry){return group.first_seen-entry.first>=20;});
   ac->alt_history.emplace_back(group.first_seen,*decoded->altitude);
   double dt=group.first_seen-ac->alt_history.front().first;
   if(dt>10){double rate=(*decoded->altitude-ac->alt_history.front().second)/(dt/60);
    if(ac->vrate&&*ac->vrate&&group.first_seen-ac->vrate_time<15)rate=*ac->vrate+.3*(rate-*ac->vrate);
    ac->vrate=static_cast<int>(rate);ac->vrate_time=group.first_seen;}
  }
 }
 if(decoded->squawk)ac->squawk=decoded->squawk;
 if(decoded->callsign)ac->callsign=decoded->callsign;
 if(now-ac->last_resolve_attempt<.15)return;
 ac->last_resolve_attempt=now;
 auto last_position=ac->last_result_position;double last_time=ac->last_result_time;int last_dof=ac->last_result_dof;
 if(!last_position||group.first_seen-last_time>120){last_position.reset();last_time=group.first_seen-120;last_dof=0;}
 double elapsed=std::max(0.0,group.first_seen-last_time);if(elapsed<.7)return;
 std::optional<double> altitude;
 // Preserve baseline's feet-versus-metre range comparison before conversion.
 if(ac->altitude&&*ac->altitude>=-1500*constants::ftom&&*ac->altitude<=75000*constants::ftom&&group.first_seen<=ac->last_altitude_time+45)altitude=*ac->altitude*constants::ftom;
 int altitude_dof=altitude?1:0;
 int max_dof=static_cast<int>(group.copies.size())+altitude_dof-4;
 if(max_dof<0||(elapsed<1.4&&max_dof<last_dof-elapsed+.5))return;
 std::vector<clock::StationSamples> samples;std::map<clock::StationId,std::size_t> indices;std::vector<clock::StationId> ids;
 for(const auto& c:group.copies) {
  auto [it,inserted]=indices.emplace(c.receiver->id,samples.size());
  if(inserted){samples.push_back({c.receiver->id,c.receiver->clock,{}});ids.push_back(c.receiver->id);}
  samples[it->second].samples.push_back({c.timestamp,c.utc});
 }
 int dof=static_cast<int>(samples.size())+altitude_dof-4;
 if(dof<0||(elapsed<1.4&&dof<last_dof-elapsed+.5))return;
 ++stats_.normalize;
 std::vector<clock::NormalizedStation> component;
 try{component=clock::normalize(samples,pairs_(ids),now);}catch(const std::exception&){return;}
 const int minimum=4-altitude_dof;
 if(component.size()<static_cast<std::size_t>(minimum))return;
 auto clusters=cluster_timestamps(component,group.receivers,minimum);
 std::stable_sort(clusters.begin(),clusters.end(),[](const auto& a,const auto& b){return a.distinct!=b.distinct?a.distinct<b.distinct:a.first_seen<b.first_seen;});
 while(!clusters.empty()) {
  Cluster cluster=std::move(clusters.back());clusters.pop_back();
  elapsed=cluster.first_seen-last_time;dof=cluster.distinct+altitude_dof-4;
  if(elapsed<2&&dof<last_dof-elapsed+.5)return;
  std::optional<double> altitude_error;
  if(decoded->altitude)altitude_error=250*constants::ftom;
  else if(altitude)altitude_error=(250+(cluster.first_seen-ac->last_altitude_time)*70)*constants::ftom;
  std::stable_sort(cluster.samples.begin(),cluster.samples.end(),[](const auto& a,const auto& b){return a.timestamp<b.timestamp;});
  if(elapsed>30&&dof==0)continue;
  Vec3 guess=elapsed<60&&last_position?*last_position:cluster.samples[0].receiver->position;
  ++stats_.solve_attempt;std::vector<Measurement> measurements;
  for(const auto& s:cluster.samples)measurements.push_back({s.receiver->position,s.timestamp,s.variance});
  auto result=solve(measurements,altitude,altitude_error,guess);if(!result)continue;
  auto error=tracker_position_error(*result);if(!error||*error>10000)continue;
  ++stats_.solve_success;if(elapsed/20<*error/10000)continue;
  ++stats_.solve_used;ac->last_result_position=result->position;
  const auto& cov=*result->covariance;ac->last_result_var=cov[0][0]+cov[1][1]+cov[2][2];
  ac->last_result_time=cluster.first_seen;ac->last_result_dof=dof;++ac->mlat_result_count;
  std::optional<double> filter_altitude=altitude,filter_error=altitude_error;
  if(altitude){auto llh=ecef_to_llh(result->position);llh[2]=*altitude;result->position=llh_to_ecef(llh);}
  else {filter_altitude=ecef_to_llh(result->position)[2];filter_error=4000/std::sqrt(dof+1);}
  if(ac->kalman.update(cluster.first_seen,measurements,filter_altitude,filter_error,result->position,cov,dof))++ac->mlat_kalman_count;
  MlatResult output{cluster.first_seen,decoded->address,*result,{},{},cluster.distinct,dof,ac->kalman.state(),*error};
  for(const auto& s:cluster.samples)output.cluster_receivers.push_back(s.receiver);
  for(const auto& [id,r]:group.receivers){(void)id;output.all_receivers.push_back(r);}
  if(output_)output_(output);
  return;
 }
}
}
