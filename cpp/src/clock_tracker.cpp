// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of clocktrack.pyx, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/clock_tracker.hpp"
#include "mlat/modes.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace mlat {
std::size_t ClockTracker::Hash::operator()(const Key& k)const noexcept {
 std::size_t h=1469598103934665603ULL;
 for(auto b:k.a){h^=b;h*=1099511628211ULL;}for(auto b:k.b){h^=b;h*=1099511628211ULL;}return h;
}
ClockTracker::~ClockTracker(){for(auto& [id,weak]:receivers_){(void)id;if(auto r=weak.lock())r->on_clock_reset={};}}
void ClockTracker::attach(const std::shared_ptr<ReceiverModel>& r) {
 if(!r)throw std::invalid_argument("null receiver");
 auto found=receivers_.find(r->id);
 if(found!=receivers_.end()) {if(found->second.lock()!=r)throw std::invalid_argument("reused receiver UID");return;}
 if(highest_uid_&&r->id<=*highest_uid_)throw std::invalid_argument("receiver UIDs must increase");
 for(double v:r->position)if(!std::isfinite(v))throw std::invalid_argument("invalid receiver position");
 receivers_.emplace(r->id,r);highest_uid_=r->id;std::weak_ptr<ReceiverModel> weak=r;
 r->on_clock_reset=[this,weak]{if(auto receiver=weak.lock())clock_reset(*receiver,now_);};
}
void ClockTracker::change_peers(ReceiverModel& r,int cat,int delta) {
 r.sync_peers[static_cast<std::size_t>(cat)]+=delta;
 r.total_sync_peers=std::accumulate(r.sync_peers.begin(),r.sync_peers.end(),0);
}
void ClockTracker::remove_pair(std::map<PairKey,std::unique_ptr<Pair>>::iterator it) {
 auto& p=*it->second;change_peers(*p.base,p.category,-1);change_peers(*p.peer,p.category,-1);pairs_.erase(it);
}
void ClockTracker::cleanup(double now) {
 if(!std::isfinite(now))throw std::invalid_argument("invalid cleanup time");
 now_=now;
 for(auto it=pairs_.begin();it!=pairs_.end();) {
  auto& p=it->second->model;p.check_valid(now);const auto& s=p.state();
  if(now-s.updated>45||(!s.valid&&now-s.updated>30)){auto old=it++;remove_pair(old);}else ++it;
 }
}
void ClockTracker::clock_reset(ReceiverModel& r,double now) {
 if(!std::isfinite(now))throw std::invalid_argument("invalid reset time");
 now_=now;
 for(auto& [key,p]:pairs_)if(key.first==r.id||key.second==r.id)p->model.reset_offsets();
 r.last_clock_reset=now;++r.clock_reset_counter;
}
void ClockTracker::disconnect(ReceiverModel& r) {
 r.dead=true;
 for(auto it=pairs_.begin();it!=pairs_.end();)if(it->first.first==r.id||it->first.second==r.id){auto old=it++;remove_pair(old);}else ++it;
 r.on_clock_reset={};receivers_.erase(r.id);
}
void ClockTracker::add(Details& d,Point& point,const std::shared_ptr<ReceiverModel>& r0,double a,double b,double now) {
 const double range_a=ecef_distance(d.a,r0->position),range_b=ecef_distance(d.b,r0->position);
 if(range_a>500e3){r0->sync_range_exceeded=now;return;}
 if(shards_>=1000000){++stats_.bounded_drops;return;}
 double delay=r0->clock.frequency/constants::cair;
 double ta=a-range_a*delay,tb=b-range_b*delay,interval=tb-ta;
 constexpr int limits[]{32,32,16,16};
 for(const auto& other:point.receivers) {
  const auto& r1=other.receiver;
  PairKey key=std::minmax(r0->id,r1->id);auto found=pairs_.find(key);
  if(found==pairs_.end()) {
   if(r1->dead||r0==r1)continue;
   int cat=std::min(3,static_cast<int>(ecef_distance(r0->position,r1->position)/50e3));
   double limit=0.7*limits[cat];
   if(r0->sync_peers[static_cast<std::size_t>(cat)]>limit&&r1->sync_peers[static_cast<std::size_t>(cat)]>limit)continue;
   // Preserve pinned source's arrival-order constructor despite sorted map key.
   found=pairs_.emplace(key,std::make_unique<Pair>(r0,r1,cat,++pair_sequence_)).first;
   change_peers(*r0,cat,1);change_peers(*r1,cat,1);
  }else {
   const auto& s=found->second->model.state();int cat=found->second->category;
   if(s.n>8&&now-s.update_attempted<0.2)continue;
   double limit=1.2*limits[cat];
   if(r0->sync_peers[static_cast<std::size_t>(cat)]>limit&&r1->sync_peers[static_cast<std::size_t>(cat)]>limit){remove_pair(found);continue;}
  }
  auto& model=found->second->model;
  if(r0->id<r1->id)model.update(tb,other.timestamp,interval,other.interval,now,*d.aircraft);
  else model.update(other.timestamp,tb,other.interval,interval,now,*d.aircraft);
 }
 point.receivers.push_back({r0,tb,interval});++shards_;
}
void ClockTracker::sync(const std::shared_ptr<ReceiverModel>& r,double even_time,double odd_time,
                       const SyncMessage& even,const SyncMessage& odd,double now) {
 if(!r||!std::isfinite(even_time)||!std::isfinite(odd_time)||!std::isfinite(now))throw std::invalid_argument("invalid sync input");
 if(r->dead)return;
 attach(r);now_=now;++stats_.messages;
 double a=std::min(even_time,odd_time),b=std::max(even_time,odd_time);
 Key key=even_time<odd_time?Key{even,odd}:Key{odd,even};
 auto found=points_.find(key);
 if(found!=points_.end()) {
  auto& d=found->second;
  if(d.do_mlat&&mlat_)mlat_(*r,even_time,even,now,false);
  if(!d.valid)return;
 }
 double interval=(b-a)/r->clock.frequency;if(interval>5)return;
 if(found!=points_.end()&&!found->second.points.empty()) {
  auto& d=found->second;
  for(auto& point:d.points)if(std::abs(point.interval-interval)<0.75e-3){add(d,point,r,a,b,now);return;}
  if(d.points.size()>=4096){++stats_.bounded_drops;return;}
  d.points.push_back({interval,{}});add(d,d.points.back(),r,a,b,now);return;
 }
 if(now-r->sync_range_exceeded<15)return;
 std::optional<modes::Message> em,om;
 try {em=modes::decode(even);om=modes::decode(odd);}catch(const std::invalid_argument&){return;}
 if(!em||!om)return;
 auto ac=lookup_(em->address);if(!ac)return;ac->seen=now;
 bool do_mlat=(ac->do_mlat||ac->force_mlat)&&ac->tracking.size()>=3;
 if(do_mlat&&mlat_)mlat_(*r,even_time,even,now,true);
 if(points_.size()>=250000){++stats_.bounded_drops;return;}
 auto [it,inserted]=points_.insert_or_assign(key,Details{ac,do_mlat,false,{},{},{}});(void)inserted;
 auto& d=it->second;
 if(em->df!=17||em->crc_ok!=true||em->f!=0||om->df!=17||om->crc_ok!=true||om->f!=1||em->address!=om->address)return;
 if(em->estype!=modes::ESType::airborne_position||om->estype!=modes::ESType::airborne_position) {
  if(em->estype==modes::ESType::surface_position&&om->estype==modes::ESType::surface_position)ac->last_adsb_time=now;
  return;
 }
 std::array<double,4> ll;
 try{ll=modes::decode_cpr(*em->lat,*em->lon,*om->lat,*om->lon);}catch(const std::invalid_argument&){return;}
 if(!em->altitude||!om->altitude||std::abs(*em->altitude-*om->altitude)>5000)return;
 if(ll[0]>85||ll[0]<-85||ll[0]==0||ll[1]==0)return;
 Vec3 ep=llh_to_ecef({ll[0],ll[1],*em->altitude*constants::ftom}),op=llh_to_ecef({ll[2],ll[3],*om->altitude*constants::ftom});
 if(ecef_distance(ep,op)>10e3)return;
 ac->last_altitude_time=now;ac->altitude=em->altitude;
 if(em->nuc.value_or(0)<6||om->nuc.value_or(0)<6)return;
 d.a=even_time<odd_time?ep:op;d.b=even_time<odd_time?op:ep;d.valid=true;d.points.push_back({interval,{}});++stats_.points;
 if(ecef_distance(ep,r->position)>500e3){r->sync_range_exceeded=now;return;}
 add(d,d.points.back(),r,a,b,now);
}
std::vector<PairSnapshot> ClockTracker::pairs()const {
 std::vector<PairSnapshot> result;result.reserve(pairs_.size());
 for(const auto& [key,p]:pairs_)result.push_back({key.first,key.second,p->category,p->model.state()});
 return result;
}
std::vector<clock::PairModel> ClockTracker::normalization_models()const {
 std::vector<clock::PairModel> result;result.reserve(pairs_.size());
 for(const auto& [key,p]:pairs_)result.push_back({key.first,key.second,p->model.state()});
 return result;
}
std::vector<PairSnapshot> ClockTracker::status_pairs()const {
 std::vector<std::pair<std::uint64_t,PairSnapshot>> ordered;ordered.reserve(pairs_.size());
 for(const auto& [key,p]:pairs_)ordered.emplace_back(p->sequence,PairSnapshot{key.first,key.second,p->category,p->model.state()});
 std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.first<b.first;});
 std::vector<PairSnapshot> result;result.reserve(ordered.size());for(auto& entry:ordered)result.push_back(std::move(entry.second));return result;
}
void ClockTracker::decay_status_counters(){for(auto& [key,p]:pairs_){(void)key;p->model.decay_status_counters();}}
std::vector<clock::PairModel> ClockTracker::normalization_models(std::span<const clock::StationId> stations)const {
 if(stations.size()>4096)throw std::length_error("normalization subset bound");
 std::vector<clock::StationId> ids(stations.begin(),stations.end());std::sort(ids.begin(),ids.end());
 ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
 std::vector<clock::PairModel> result;
 for(std::size_t i=0;i<ids.size();++i)for(std::size_t j=i+1;j<ids.size();++j) {
  auto found=pairs_.find({ids[i],ids[j]});if(found!=pairs_.end())result.push_back({ids[i],ids[j],found->second->model.state()});
 }
 return result;
}
}
