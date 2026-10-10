# ADSBiq MLAT C++

**A correctness-first C++20 multilateration server for Linux.**

Built by Sky Power Services for ADSBiq, this experimental port aims to preserve the behavior of the established Python/Cython MLAT server while measuring what a native C++ implementation can deliver.

> **Development preview:** implementation and validation are in progress. Reviewed source and build instructions are being prepared for publication. This README is not a release or a complete source distribution. No production-readiness or performance advantage is claimed.

## What it does

Multilateration estimates aircraft positions from the differences in arrival times of radio messages received at geographically separated stations. This server brings the processing pipeline together:

```text
Receiver connections
        |
Protocol decoding and receiver clock synchronization
        |
Observation grouping and position solving
        |
Kalman filtering and recipient routing
        |
JSON / SBS output and CSV recording
```

The implementation includes compressed TCP sessions, receiver interest management, clock tracking, numerical solving, filtering, status reporting, and bounded output handling.

## Validation, not assumptions

Development validation as of October 10, 2026:

| Check | Observed result |
| --- | --- |
| C++ Release test suite | 22 of 22 tests passed |
| AddressSanitizer + UndefinedBehaviorSanitizer suite | 22 of 22 tests passed |
| Controlled six-receiver Python/C++ TCP comparison | All 170 recipient result messages matched |
| Same controlled comparison: start/stop, statistics, heartbeat messages | All 36 scored control messages matched |

These are correctness checks on controlled fixtures, **not a real-network speed benchmark or proof of complete parity**. The 170 messages represent recipient deliveries, not 170 independent position solutions. Controlled receipt timing is not a service-latency measurement.

Next milestones are safe real-workload capture, repeated full-service comparisons on matched Linux hardware, and sustained stability testing. Performance reporting will include CPU, memory, throughput, latency, and output fidelity. Language choice alone is not a benchmark.

## Build direction

The current implementation targets Linux with C++20 and CMake 3.22 or newer. Dependencies include zlib, nlohmann/json, CMINPACK, LAPACKE, and threads. Reproducible build and test instructions will accompany the reviewed source publication.

## License and upstream credit

This is a port of **wiedehopf/mlat-server**, based on revision `bcfacbf67a8d592101b975f7529f78294019c2bd`. Credit belongs to Oliver Jowett and the upstream contributors whose work underpins this project.

The overall port uses **AGPL-3.0-or-later**. Geodesy/constants files retain their **GPL-3.0-or-later** notices; upstream and dependency notices will be preserved in the source distribution.

This product includes software developed by the University of Chicago, as Operator of Argonne National Laboratory.

Public source publication excludes private feeder data, operational captures, and credentials.
