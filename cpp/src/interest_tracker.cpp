// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of tracker.py / Receiver interest methods, Oliver Jowett and contributors.
#include "mlat/interest_tracker.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <tuple>
namespace mlat {
namespace {
constexpr std::size_t max_aircraft=100000,max_memberships=1000000,max_receivers=4096,max_ratepairs=1000000;
void validate_indices(const std::vector<std::size_t>& indices,std::size_t n,std::size_t k){
 if(indices.size()!=k)throw std::invalid_argument("random choice count");
 std::set<std::size_t> used;
 for(auto i:indices)if(i>=n||!used.insert(i).second)throw std::invalid_argument("invalid random choice");
}
}
double SeededInterestRandom::uniform(){return std::generate_canonical<double,53>(engine_);}
std::vector<std::size_t> SeededInterestRandom::sample(std::size_t n,std::size_t k){
 if(n>max_ratepairs||k>n)throw std::invalid_argument("random sample bound");
 std::vector<std::size_t> indices(n);std::iota(indices.begin(),indices.end(),0);
 for(std::size_t i=0;i<k;++i){std::uniform_int_distribution<std::size_t> d(i,n-1);std::swap(indices[i],indices[d(engine_)]);}
 indices.resize(k);return indices;
}
std::vector<std::size_t> SeededInterestRandom::shuffle(std::size_t n){return sample(n,n);}
InterestTracker::InterestTracker(InterestRandom& random,Refresh schedule,double now,InterestOptions options):
 random_(random),schedule_(std::move(schedule)),options_(options),wanted_time_(now){
 if(!std::isfinite(now)||!options.partitions||!options.partition||options.partition>options.partitions||!options.min_tracking||options.min_tracking>max_receivers)
  throw std::invalid_argument("invalid interest configuration");
}
void InterestTracker::attach(std::shared_ptr<ReceiverModel> r){
 if(!r||r->dead||(high_uid_&&r->id<=*high_uid_)||!r->tracking.empty())throw std::invalid_argument("invalid interest receiver generation");
 if(receivers_.size()>=max_receivers)throw std::length_error("interest receiver bound");
 high_uid_=r->id;receivers_.emplace(r->id,std::move(r));
}
void InterestTracker::check_receiver(const ReceiverModel& r)const{
 auto it=receivers_.find(r.id);if(it==receivers_.end()||it->second.get()!=&r)throw std::invalid_argument("unregistered interest receiver");
}
bool InterestTracker::in_local_partition(std::uint32_t address)const noexcept{
 if(options_.partitions==1)return true;
 auto h=address;h=((h>>16)^h)*0x45d9f3bU;h=((h>>16)^h)*0x45d9f3bU;h=(h>>16)^h;
 return h%options_.partitions==options_.partition-1;
}
std::shared_ptr<AircraftModel> InterestTracker::find(std::uint32_t address)const{
 auto it=aircraft_.find(address);return it==aircraft_.end()?nullptr:it->second;
}
double InterestTracker::uniform(){double v=random_.uniform();if(!std::isfinite(v)||v<0||v>=1)throw std::invalid_argument("invalid random uniform");return v;}
void InterestTracker::add(ReceiverModel& r,std::span<const std::uint32_t> addresses,double now){
 check_receiver(r);if(!std::isfinite(now)||addresses.size()>max_aircraft)throw std::invalid_argument("invalid tracking input");
 std::set<std::uint32_t> new_addresses;std::size_t memberships=0;
 for(auto id:addresses){if(id>0xffffff)throw std::invalid_argument("invalid aircraft address");if(new_addresses.insert(id).second){auto ac=find(id);if(!ac||!ac->tracking.contains(r.id))++memberships;}}
 std::size_t creates=0;for(auto id:new_addresses)if(!aircraft_.contains(id))++creates;
 if(aircraft_.size()+creates>max_aircraft||tracking_memberships_+memberships>max_memberships)throw std::length_error("tracking state bound");
 // Preserve caller order for aircraft creation (and uniform draw assignment).
 for(auto id:addresses){auto ac=find(id);if(!ac){ac=std::make_shared<AircraftModel>();ac->address=id;ac->generation=++next_generation_;
   ac->allow_mlat=in_local_partition(id);ac->last_force_mlat=now-600*uniform();aircraft_.emplace(id,ac);creation_order_.emplace(ac->generation,ac);}
  if(ac->tracking.insert(r.id).second)++tracking_memberships_;
  r.tracking.insert(ac);ac->seen=now;
 }
}
void InterestTracker::erase_active(const std::shared_ptr<AircraftModel>& ac){
 auto it=aircraft_.find(ac->address);if(it!=aircraft_.end()&&it->second==ac){aircraft_.erase(it);creation_order_.erase(ac->generation);}
}
void InterestTracker::remove(ReceiverModel& r,std::span<const std::uint32_t> addresses){
 check_receiver(r);if(addresses.size()>max_aircraft)throw std::length_error("lost list bound");
 for(auto id:addresses)if(auto ac=find(id)){tracking_memberships_-=ac->tracking.erase(r.id);r.tracking.erase(ac);if(ac->tracking.empty())erase_active(ac);}
 // Baseline leaves interest sets unchanged until next refresh. Keep generations.
}
void InterestTracker::disconnect(ReceiverModel& r){
 check_receiver(r);
 for(const auto& ac:r.tracking){tracking_memberships_-=ac->tracking.erase(r.id);ac->sync_interest.erase(r.id);ac->adsb_seen.erase(r.id);ac->mlat_interest.erase(r.id);if(ac->tracking.empty())erase_active(ac);}
 r.tracking.clear();r.adsb_seen.clear();r.sync_interest.clear();r.mlat_interest.clear();receivers_.erase(r.id);
}
AircraftSet InterestTracker::sample(const AircraftSet& candidates,std::size_t count){
 std::vector<std::shared_ptr<AircraftModel>> v(candidates.begin(),candidates.end());
 auto indices=random_.sample(v.size(),count);validate_indices(indices,v.size(),count);
 AircraftSet selected;for(auto i:indices)selected.insert(v[i]);return selected;
}
void InterestTracker::set_interest(ReceiverModel& r,AircraftSet sync,AircraftSet mlat,AircraftSet adsb){
 if(r.bad_syncs>3&&sync.size()>3)sync=sample(sync,3);
 if(r.bad_syncs>0)mlat.clear();
 auto update=[&](AircraftSet& old,const AircraftSet& next,auto member){
  for(const auto& ac:next)if(!old.contains(ac))(ac.get()->*member).insert(r.id);
  for(const auto& ac:old)if(!next.contains(ac))(ac.get()->*member).erase(r.id);
  old=next;
 };
 update(r.adsb_seen,adsb,&AircraftModel::adsb_seen);update(r.sync_interest,sync,&AircraftModel::sync_interest);update(r.mlat_interest,mlat,&AircraftModel::mlat_interest);
 if(schedule_)schedule_(receivers_.at(r.id));
}
void InterestTracker::refresh_requests(ReceiverModel& r){r.requested=r.sync_interest;r.requested.insert(r.mlat_interest.begin(),r.mlat_interest.end());}
void InterestTracker::update_interest(ReceiverModel& r,double now){
 check_receiver(r);if(!std::isfinite(now))throw std::invalid_argument("invalid interest time");
 if(r.last_rate_report){if(r.last_rate_report->size()>max_aircraft)throw std::length_error("rate report bound");for(const auto& [id,rate]:*r.last_rate_report)if(id>0xffffff||!std::isfinite(rate)||rate<0||rate>1000000)throw std::invalid_argument("invalid aircraft rate");}
 if(now-wanted_time_>.1){
  wanted_.clear();for(const auto& [generation,ac]:creation_order_){(void)generation;double since=now-ac->last_force_mlat;
   if(!ac->force_mlat&&since>585)ac->force_mlat=true;
   if(since>615){ac->last_force_mlat=now+uniform();ac->force_mlat=false;}
   ac->do_mlat=ac->tracking.size()>=options_.min_tracking&&ac->allow_mlat&&(now-ac->last_adsb_time>120||ac->sync_bad_percent>10||(since>585&&since<600));
   if(ac->do_mlat)wanted_.insert(ac);
  }wanted_time_=now;
 }
 AircraftSet mlat,adsb,sync;for(const auto& ac:r.tracking)if(wanted_.contains(ac))mlat.insert(ac);
 if(!r.last_rate_report){sync=r.tracking;if(sync.size()>15)sync=sample(sync,15);set_interest(r,std::move(sync),std::move(mlat),{});return;}
 struct RatePair{double product;clock::StationId receiver;std::shared_ptr<AircraftModel> ac;double rate;};
 std::vector<RatePair> pairs;std::map<std::uint64_t,std::vector<std::size_t>> by_aircraft;AircraftSet reported;
 for(const auto& [address,rate]:*r.last_rate_report){auto ac=find(address);if(!ac)continue;ac->seen=now;if(options_.mark_adsb_time)ac->last_adsb_time=now;adsb.insert(ac);reported.insert(ac);
  auto& indices=by_aircraft[ac->generation];double factor=ac->altitude&&*ac->altitude>0?1+std::pow(*ac->altitude/20000.0,1.5):1;
  for(auto peer_id:ac->tracking){if(peer_id==r.id)continue;const auto& peer=*receivers_.at(peer_id);double rate1=.8;
   if(peer.last_rate_report){auto it=peer.last_rate_report->find(address);rate1=it==peer.last_rate_report->end()?0:it->second;}
   double product=rate*rate1/2.25*factor;if(product<.01)continue;
   if(!std::isfinite(product))throw std::invalid_argument("invalid rate product");
   if(pairs.size()>=max_ratepairs)throw std::length_error("rate pair bound");
   indices.push_back(pairs.size());pairs.push_back({product,peer_id,ac,rate});
  }
 }
 std::vector<std::size_t> order(pairs.size());std::iota(order.begin(),order.end(),0);
 std::sort(order.begin(),order.end(),[&](auto a,auto b){const auto& x=pairs[a];const auto& y=pairs[b];return std::tie(x.product,x.receiver,x.ac->address,x.rate)>std::tie(y.product,y.receiver,y.ac->address,y.rate);});
 std::size_t half=order.size()/2;auto shuffled=random_.shuffle(half);validate_indices(shuffled,half,half);
 std::map<clock::StationId,double> totals;double total_rate=0;
 auto select=[&](std::size_t index,double threshold,bool first){const auto& p=pairs[index];if(sync.contains(p.ac)||(first&&p.ac->sync_dont_use))return true;
  if(total_rate>12)return false;
  if(totals[p.receiver]<threshold){sync.insert(p.ac);total_rate+=p.rate;for(auto j:by_aircraft.at(p.ac->generation))totals[pairs[j].receiver]+=pairs[j].product;}
  return true;
 };
 for(auto i:shuffled)if(!select(order[i],.3,true))break;
 for(auto i:order)if(!select(i,3.5,false))break;
 auto fill=[&](const AircraftSet& available){if(sync.size()>=3)return;AircraftSet candidates;for(const auto& ac:available)if(!sync.contains(ac))candidates.insert(ac);auto selected=sample(candidates,std::min(candidates.size(),3-sync.size()));sync.insert(selected.begin(),selected.end());};
 fill(reported);fill(r.tracking);set_interest(r,std::move(sync),std::move(mlat),std::move(adsb));
}
}
