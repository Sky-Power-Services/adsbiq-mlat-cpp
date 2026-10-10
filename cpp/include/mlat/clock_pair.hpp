// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of clocktrack.pyx, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include <array>
#include <functional>
#include <string_view>

namespace mlat::clock {
struct Clock {double frequency, max_frequency_error, jitter;};
Clock make_clock(std::string_view type);
struct Receiver {
    Clock clock;
    double last_sync=0, bad_syncs=0, num_syncs=0;
    int recent_pair_jumps=0, total_sync_peers=0;
    std::function<void()> on_clock_reset;
    void increment_jumps();
};
struct Aircraft {
    bool sync_dont_use=false;
    double sync_good=0, sync_bad=0, last_adsb_time=0;
};
struct State {
    bool valid=false;
    double updated=0, variance=-1e-6;
    int n=0;
    double factor=0, i_factor=0, base_avg=0, peer_avg=0;
    double raw_drift=0, drift=0, i_drift=0;
    int drift_n=0, drift_outliers=0, cooldown=5;
    double outlier_total=0, update_total=1e-3;
    int outliers=0;
    double cumulative_error=0, error=-1e-6, var_sum=0;
    int jumped=0;
    double update_attempted=0, update_last_sync=0;
};
// Receivers must have stable addresses and outlive all their ClockPairs.
// Owned by one coordinator thread; no per-update allocation or mutex.
class ClockPair {
public:
    ClockPair(Receiver& base, Receiver& peer);
    bool update(double base_ts,double peer_ts,double base_interval,double peer_interval,
                double now,Aircraft& aircraft);
    bool check_valid(double now);
    void reset_offsets() noexcept;
    double predict_peer(double base_ts) const;
    double predict_base(double peer_ts) const;
    const State& state() const noexcept {return s_;}
    void decay_status_counters() noexcept {
        if(s_.n>=2){s_.outlier_total/=2;s_.update_total/=2;s_.jumped=0;}
    }
private:
    bool update_drift(double base_interval,double peer_interval);
    void update_offset(double base_ts,double peer_ts,double prediction_error);
    void prune(double now);
    Receiver &base_, &peer_;
    double relative_frequency_, inverse_relative_frequency_, drift_max_, drift_max_delta_;
    std::array<double,32> ts_base_{}, ts_peer_{}, variance_{};
    State s_{};
};
}
