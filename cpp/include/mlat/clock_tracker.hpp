// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of clocktrack.pyx, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/models.hpp"
#include <map>
#include <unordered_map>
namespace mlat {
using SyncMessage=std::array<std::uint8_t,14>;
struct PairSnapshot {clock::StationId first,second;int category;clock::State state;};
struct SyncStats {std::uint64_t messages=0,points=0,bounded_drops=0;};
class ClockTracker {
public:
 using AircraftLookup=std::function<std::shared_ptr<AircraftModel>(std::uint32_t)>;
 // decoded=true preserves upstream's initial decoded-object MLAT event versus
 // subsequent raw-byte events; integration must not merge those pending keys.
 using MlatCallback=std::function<void(ReceiverModel&,double,const SyncMessage&,double,bool)>;
 ClockTracker(AircraftLookup lookup,MlatCallback callback):lookup_(std::move(lookup)),mlat_(std::move(callback)){}
 ~ClockTracker();
 ClockTracker(const ClockTracker&)=delete;ClockTracker& operator=(const ClockTracker&)=delete;
 void attach(const std::shared_ptr<ReceiverModel>& receiver);
 void sync(const std::shared_ptr<ReceiverModel>& receiver,double even_time,double odd_time,
           const SyncMessage& even,const SyncMessage& odd,double now);
 void cleanup(double now);
 void clock_reset(ReceiverModel& receiver,double now);
 void disconnect(ReceiverModel& receiver);
 void clear_sync_points()noexcept {points_.clear();shards_=0;}
 std::vector<PairSnapshot> pairs()const;
 std::vector<PairSnapshot> status_pairs()const;
 std::vector<clock::PairModel> normalization_models()const;
 std::vector<clock::PairModel> normalization_models(std::span<const clock::StationId> stations)const;
 // Called once per actual status dump, not on ordinary read-only inspection.
 void decay_status_counters();
 std::size_t pending_keys()const noexcept{return points_.size();}
 const SyncStats& stats()const noexcept{return stats_;}
private:
 using PairKey=std::pair<clock::StationId,clock::StationId>;
 struct Key {SyncMessage a,b;bool operator==(const Key&)const=default;};
 struct Hash {std::size_t operator()(const Key& key)const noexcept;};
 struct Shard {std::shared_ptr<ReceiverModel> receiver;double timestamp,interval;};
 struct Point {double interval;std::vector<Shard> receivers;};
 struct Details {std::shared_ptr<AircraftModel> aircraft;bool do_mlat=false,valid=false;Vec3 a{},b{};std::vector<Point> points;};
 struct Pair {std::shared_ptr<ReceiverModel> base,peer;int category;std::uint64_t sequence;clock::ClockPair model;
  Pair(std::shared_ptr<ReceiverModel> a,std::shared_ptr<ReceiverModel> b,int c,std::uint64_t order):base(std::move(a)),peer(std::move(b)),category(c),sequence(order),model(*base,*peer){}
 };
 void add(Details& details,Point& point,const std::shared_ptr<ReceiverModel>& receiver,double a,double b,double now);
 void remove_pair(std::map<PairKey,std::unique_ptr<Pair>>::iterator it);
 static void change_peers(ReceiverModel& receiver,int category,int delta);
 AircraftLookup lookup_;MlatCallback mlat_;
 std::map<PairKey,std::unique_ptr<Pair>> pairs_;
 std::unordered_map<Key,Details,Hash> points_;
 std::map<clock::StationId,std::weak_ptr<ReceiverModel>> receivers_;
 std::optional<clock::StationId> highest_uid_;
 SyncStats stats_{};double now_=0;std::size_t shards_=0;
 std::uint64_t pair_sequence_=0;
};
}
