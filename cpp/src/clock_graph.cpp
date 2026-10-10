// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of clocktrack.pyx, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/clock_graph.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
namespace mlat::clock {
namespace {
struct Predictor {double target,source,factor,variance;};
struct Edge {std::size_t a,b; Predictor ab,ba;};
struct Neighbor {std::size_t node,edge;};
}
std::vector<NormalizedStation> normalize(const std::vector<StationSamples>& input,
                                        const std::vector<PairModel>& pairs,double now) {
 if(!std::isfinite(now)) throw std::invalid_argument("nonfinite normalization time");
 // A bounded sparse graph, not a dense network-sized matrix.
 if(input.size()>4096 || pairs.size()>1000000) throw std::length_error("clock graph bound");
 std::vector<const StationSamples*> stations;
 for(const auto& s:input) stations.push_back(&s);
 std::sort(stations.begin(),stations.end(),[](auto a,auto b){return a->id<b->id;});
 const auto n=stations.size();
 std::unordered_map<StationId,std::size_t> index;
 for(std::size_t i=0;i<n;++i) {
  const auto& s=*stations[i];
  if(!index.emplace(s.id,i).second) throw std::invalid_argument("duplicate station");
  if(!std::isfinite(s.clock.frequency)||s.clock.frequency<=0||!std::isfinite(s.clock.jitter)||s.clock.jitter<0)
   throw std::invalid_argument("invalid normalization clock");
  for(const auto& ts:s.samples) if(!std::isfinite(ts.ticks)||!std::isfinite(ts.utc))
   throw std::invalid_argument("nonfinite timestamp");
 }
 std::vector<Edge> edges;
 for(const auto& p:pairs) {
  if(p.base>=p.peer) throw std::invalid_argument("unordered clock pair");
  auto a=index.find(p.base),b=index.find(p.peer);
  if(a==index.end()||b==index.end()||!p.state.valid) continue;
  const auto& s=p.state;
  if(s.n<1 || s.n>32) throw std::invalid_argument("invalid clock sample count");
  double variance=s.variance*(1+(now-s.updated)/60.0);
  if(s.n<10) variance*=1+(10-s.n)*0.05;
  if(!std::isfinite(variance)||variance<0||!std::isfinite(s.base_avg)||!std::isfinite(s.peer_avg)
     ||!std::isfinite(s.factor)||s.factor<=0||!std::isfinite(s.i_factor)||s.i_factor<=0)
   throw std::invalid_argument("invalid clock predictor");
  edges.push_back({a->second,b->second,{s.peer_avg,s.base_avg,s.factor,variance},
                                    {s.base_avg,s.peer_avg,s.i_factor,variance}});
 }
 // CSR row order controls equal-weight tie order in SciPy's stable Kruskal sort.
 std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return a.a!=b.a?a.a<b.a:a.b<b.b;});
 for(std::size_t i=1;i<edges.size();++i) if(edges[i-1].a==edges[i].a&&edges[i-1].b==edges[i].b)
  throw std::invalid_argument("duplicate pair model");
 if(edges.size()<2) return {};
 std::vector<std::size_t> order(edges.size()),parent(n);
 std::iota(order.begin(),order.end(),0); std::iota(parent.begin(),parent.end(),0);
 std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return edges[a].ab.variance<edges[b].ab.variance;});
 auto root=[&](std::size_t a){while(parent[a]!=a){parent[a]=parent[parent[a]];a=parent[a];}return a;};
 std::vector<bool> selected(edges.size(),false);
 for(auto k:order) {
  const auto& e=edges[k]; auto a=root(e.a),b=root(e.b);
  if(a!=b) {parent[b]=a;selected[k]=true;}
 }
 // Sparse SciPy MST drops explicit zero-weight entries from its result.
 // Recompute components after that elimination, even if zero edges joined DSU.
 std::iota(parent.begin(),parent.end(),0);
 std::vector<std::vector<Neighbor>> graph(n);
 for(std::size_t k=0;k<edges.size();++k) if(selected[k]&&edges[k].ab.variance!=0) {
  const auto& e=edges[k];parent[root(e.b)]=root(e.a);
  graph[e.a].push_back({e.b,k});graph[e.b].push_back({e.a,k});
 }
 std::vector<std::size_t> sizes(n,0);
 for(std::size_t i=0;i<n;++i) ++sizes[root(i)];
 std::size_t first=0,biggest=0;
 for(std::size_t i=0;i<n;++i) if(sizes[root(i)]>biggest){first=i;biggest=sizes[root(i)];}
 if(biggest<3) return {};
 // Iterative postorder reproduces height labels without recursion depth risk.
 std::vector<double> heights(n,0);
 std::vector<std::size_t> traversal{first},previous(n,n);
 previous[first]=first;
 for(std::size_t i=0;i<traversal.size();++i) {
  auto node=traversal[i];
  for(auto adj:graph[node]) if(previous[adj.node]==n) {previous[adj.node]=node;traversal.push_back(adj.node);}
 }
 for(auto it=traversal.rbegin();it!=traversal.rend();++it) {
  auto node=*it;
  for(auto adj:graph[node]) if(previous[adj.node]==node)
   heights[node]=std::max(heights[node],heights[adj.node]+edges[adj.edge].ab.variance);
 }
 auto tallest=[&](std::size_t node,std::size_t ignore) {
  std::pair<double,std::size_t> best{0,n};
  for(auto adj:graph[node]) if(adj.node!=ignore) {
   double h=heights[adj.node]+edges[adj.edge].ab.variance;
   if(h>best.first) best={h,adj.node};
  }
  return best;
 };
 auto t1=tallest(first,n),t2=tallest(first,t1.second);
 double target=(t1.first+t2.first)/2;
 auto central=first,step=t1.second;
 while(step!=n && std::abs(heights[central]-target)>std::abs(heights[step]-target)) {
  central=step;step=tallest(central,central).second;
 }
 struct Work {std::size_t node,parent;double variance;};
 double jitter=stations[central]->clock.jitter;
 const double seconds_factor=1/stations[central]->clock.frequency;
 std::vector<Work> stack{{central,n,jitter*jitter}};
 std::vector<std::size_t> towards_central(n,n);
 std::vector<Predictor> to_parent(n);
 std::vector<NormalizedStation> result;
 while(!stack.empty()) {
  auto w=std::move(stack.back());stack.pop_back();
  NormalizedStation out{stations[w.node]->id,w.variance,{}};
  for(auto ts:stations[w.node]->samples) {
   for(auto cursor=w.node;cursor!=central;cursor=towards_central[cursor]) {
    const auto& p=to_parent[cursor];
    ts.ticks=p.target+(ts.ticks-p.source)*p.factor;
   }
   ts.ticks=0+(ts.ticks-0)*seconds_factor;
   out.samples.push_back(ts);
  }
  result.push_back(std::move(out));
  // Preserve Python graph's insertion-order DFS output.
  for(auto it=graph[w.node].rbegin();it!=graph[w.node].rend();++it) if(it->node!=w.parent) {
   const auto& e=edges[it->edge]; const auto& p=it->node==e.a?e.ab:e.ba;
   towards_central[it->node]=w.node;to_parent[it->node]=p;
   stack.push_back({it->node,w.node,w.variance+p.variance});
  }
 }
 return result;
}
}
