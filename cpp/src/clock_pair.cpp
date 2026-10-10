// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of clocktrack.pyx, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/clock_pair.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mlat::clock {
Clock make_clock(std::string_view type) {
    if(type=="radarcape_gps" || type=="radarcape") return {1e9,1e-6,15e-9};
    if(type=="beast" || type=="radarcape_12mhz") return {12e6,5e-6,83e-9};
    if(type=="sbs") return {20e6,100e-6,500e-9};
    if(type=="dump1090" || type=="unknown") return {12e6,100e-6,500e-9};
    throw std::invalid_argument("unsupported receiver clock");
}
void Receiver::increment_jumps() {
    ++recent_pair_jumps;
    if(total_sync_peers>0 && static_cast<double>(recent_pair_jumps)/total_sync_peers>0.25 && recent_pair_jumps>3) {
        recent_pair_jumps=0;
        if(on_clock_reset) on_clock_reset();
        bad_syncs+=0.1;
    }
}
ClockPair::ClockPair(Receiver& base,Receiver& peer):base_(base),peer_(peer),
    relative_frequency_(peer.clock.frequency/base.clock.frequency),
    inverse_relative_frequency_(base.clock.frequency/peer.clock.frequency),
    drift_max_(0.75*(base.clock.max_frequency_error+peer.clock.max_frequency_error)),
    drift_max_delta_(drift_max_/20.0) {
    for(const auto& c : {base.clock,peer.clock})
        if(!std::isfinite(c.frequency) || c.frequency<=0 || !std::isfinite(c.max_frequency_error)
           || c.max_frequency_error<=0 || !std::isfinite(c.jitter) || c.jitter<0)
            throw std::invalid_argument("invalid receiver clock parameters");
}
bool ClockPair::check_valid(double now) {
    if(s_.n<2 || s_.drift_n<2 || s_.outlier_total/s_.update_total>0.5) {
        s_.variance=s_.error=-1e-6; s_.valid=false; return false;
    }
    s_.variance=s_.var_sum/s_.n;
    s_.error=std::sqrt(s_.variance);
    s_.valid=s_.cooldown<1 && s_.n>4 && s_.drift_n>4 && s_.variance<16e-12 && now-s_.updated<35;
    if(s_.valid && now>s_.update_last_sync) {
        s_.update_last_sync=now+5; base_.last_sync=peer_.last_sync=now;
    }
    return s_.valid;
}
double ClockPair::predict_peer(double t) const {
    if(s_.n==0) throw std::invalid_argument("prediction on empty clock pair");
    return s_.peer_avg+(t-s_.base_avg)*s_.factor;
}
double ClockPair::predict_base(double t) const {
    if(s_.n==0) throw std::invalid_argument("prediction on empty clock pair");
    return s_.base_avg+(t-s_.peer_avg)*s_.i_factor;
}
void ClockPair::reset_offsets() noexcept {
    s_.valid=false; s_.n=0; s_.var_sum=0; s_.error=s_.variance=-1e-6;
    s_.outliers=0; s_.cumulative_error=0; s_.base_avg=s_.peer_avg=0;
    // Deliberately retain drift and counters, as the deployed implementation does.
}
void ClockPair::prune(double now) {
    if(s_.n==0) return; // Avoid upstream ts_base[-1] undefined read for an empty pair.
    int i=std::max(0,s_.n-20);
    const double latest=ts_base_[static_cast<std::size_t>(s_.n-1)];
    while(i<s_.n && latest-ts_base_[static_cast<std::size_t>(i)]>45*base_.clock.frequency) ++i;
    if(i>0) {
        s_.n-=i;
        const auto count=static_cast<std::size_t>(s_.n)*sizeof(double);
        std::memmove(ts_base_.data(),ts_base_.data()+i,count);
        std::memmove(ts_peer_.data(),ts_peer_.data()+i,count);
        std::memmove(variance_.data(),variance_.data()+i,count);
        s_.var_sum=0;
        for(int k=0;k<s_.n;++k) s_.var_sum+=variance_[static_cast<std::size_t>(k)];
        check_valid(now);
    }
}
bool ClockPair::update_drift(double bi,double pi) {
    const double adjusted=bi*relative_frequency_;
    const double new_drift=(pi-adjusted)/adjusted;
    if(std::abs(new_drift)>drift_max_) return false;
    if(s_.drift_n<=0 || s_.drift_outliers>15) {
        s_.raw_drift=s_.drift=new_drift; s_.i_drift=-s_.drift/(1+s_.drift);
        s_.cumulative_error=0; s_.drift_outliers=0; s_.drift_n=1; return true;
    }
    const double drift_error=new_drift-s_.raw_drift;
    if(std::abs(drift_error)>drift_max_delta_) {++s_.drift_outliers;return false;}
    s_.drift_outliers=std::max(0,s_.drift_outliers-2);
    double kp=0.06;
    constexpr double ki=0.008;
    const int unstable=12-s_.drift_n;
    // Both operands are C ints under upstream cdivision=True: retain integer division.
    if(unstable>0) kp*=1+(0.2/kp)*(unstable/12);
    ++s_.drift_n;
    s_.raw_drift+=drift_error*kp;
    s_.drift=s_.raw_drift-ki*s_.cumulative_error;
    s_.i_drift=-s_.drift/(1+s_.drift);
    return true;
}
void ClockPair::update_offset(double bt,double pt,double error) {
    const auto n=static_cast<std::size_t>(s_.n);
    ts_base_[n]=bt;ts_peer_[n]=pt;variance_[n]=error*error;
    if(s_.n==0) {s_.base_avg=bt;s_.peer_avg=pt;}
    else {
        // Upstream subtracts the just-written entry, so elapsed is always zero.
        const double elapsed=bt-ts_base_[n];
        if(elapsed<20) {
            const double x=1-elapsed/20;
            const double keep=x*x*0.75;
            s_.base_avg=s_.base_avg*keep+bt*(1-keep);
            s_.peer_avg=s_.peer_avg*keep+pt*(1-keep);
        } else {s_.base_avg=bt;s_.peer_avg=pt;}
    }
    ++s_.n;s_.var_sum+=error*error;
}
bool ClockPair::update(double bt,double pt,double bi,double pi,double now,Aircraft& ac) {
    // New ingress guard: malformed floating-point values cannot poison the graph.
    if(!std::isfinite(bt)||!std::isfinite(pt)||!std::isfinite(bi)||!std::isfinite(pi)
       ||!std::isfinite(now)||bi<=0||pi<=0) return false;
    double error=0;
    bool outlier=false,do_reset=false;
    if(s_.n>31 || (s_.n>0 && bt-ts_base_[0]>50*base_.clock.frequency)) prune(now);
    if(!ac.sync_dont_use) ++s_.update_total;
    s_.update_attempted=now;
    if(s_.n>0) {
        const auto last=static_cast<std::size_t>(s_.n-1);
        if(pt<=ts_peer_[last] || bt<=ts_base_[last]) {
            if((pt<ts_peer_[last] && bt<ts_base_[last]) || pt==ts_peer_[last] || bt==ts_base_[last]) return false;
            s_.valid=false;s_.outliers+=10;outlier=true;++s_.outlier_total;
            if(s_.outliers<=10) return false;
        }
    }
    if(s_.n>0 && !outlier) {
        error=(predict_peer(bt)-pt)/peer_.clock.frequency;
        const double threshold=s_.n>=4 ? 1.5e-6 : 3e-6;
        const double absolute=std::abs(error);
        ++base_.num_syncs;++peer_.num_syncs;
        if(absolute>threshold) {
            if(peer_.bad_syncs<0.01 && base_.bad_syncs<0.01) ++ac.sync_bad;
            if(ac.sync_dont_use) return false;
            outlier=true;++s_.outlier_total;s_.outliers+=absolute>3*threshold ? 20 : 6;
            if(s_.outliers<=77 && absolute<5*threshold) return false;
            if(absolute>3*threshold) {
                do_reset=true;
                if(!s_.jumped) {
                    if(peer_.bad_syncs<0.01) base_.increment_jumps();
                    if(base_.bad_syncs<0.01) peer_.increment_jumps();
                }
                s_.jumped=1;
            }
        } else {++ac.sync_good;ac.last_adsb_time=now;}
    }
    if(ac.sync_dont_use) return false;
    if(outlier && do_reset) {reset_offsets();s_.cooldown=15;error=0;}
    s_.outliers=std::max(0,s_.outliers-18);
    if(!outlier) s_.cumulative_error=std::clamp(s_.cumulative_error+error,-50e-6,50e-6);
    s_.cooldown=std::max(0,s_.cooldown-1);
    if(!update_drift(bi,pi)) {check_valid(now);return false;}
    s_.factor=relative_frequency_*(1+s_.drift);
    s_.i_factor=inverse_relative_frequency_*(1+s_.i_drift);
    update_offset(bt,pt,error);s_.updated=now;check_valid(now);return true;
}
}
