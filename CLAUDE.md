# CLAUDE.md

## Project Overview

IEC 61850 Sample Values (SV) latency measurement tool for the SEAPATH virtualization platform. Measures NIC-to-application and NIC-to-parsed latency of SV Ethernet frames (EtherType 0x88BA) and exposes Prometheus metrics.

## Build

```bash
make            # Build all binaries (sv-subscriber, sv-capture-agent, sv-collector)
make test       # Build and run unit tests
make clean      # Remove build artifacts
make install    # Install to /usr/local/bin (PREFIX= to override)
```

Requires: Linux, GCC, pthreads. No external library dependencies (embedded HTTP server).

Build output goes to `build/`.

## Architecture

Three binaries, two deployment scenarios:

- **Scenario A (direct):** `sv-subscriber` runs alone with direct NIC access (bare-metal or SR-IOV). Captures frames, parses SV, computes latency, exposes metrics.
- **Scenario B (split):** `sv-capture-agent` (hypervisor) + `sv-subscriber --mode split` (VM) + `sv-collector` (correlator). Used when VM has no direct NIC HW timestamping.

## Source Layout

```
include/common.h          - Shared types (sv_timestamp, sv_stream_id, sv_frame_info)
src/sv_parser.{h,c}       - Minimal BER/ASN.1 SV frame parser (appID, svID, smpCnt)
src/histogram.{h,c}       - Lock-free latency histogram (1µs resolution, 0-500µs)
src/drop_detector.{h,c}   - smpCnt gap detection per stream
src/frame_capture.{h,c}   - AF_PACKET + SO_TIMESTAMPING + BPF filter + PHC clock
src/metrics.{h,c}         - Prometheus text format exporter + embedded HTTP server
src/system_monitor.{h,c}  - Link state (netlink), kernel oops (/dev/kmsg), RT throttle
src/config.{h,c}          - CLI argument parsing
src/protocol.{h,c}        - Binary batch protocol for split architecture (TCP)
src/main_subscriber.c     - sv-subscriber entry point (direct + split VM mode)
src/main_capture_agent.c  - sv-capture-agent entry point (split hypervisor mode)
src/main_collector.c      - sv-collector entry point (split correlator)
tests/test_sv_parser.c    - SV parser unit tests (plain + VLAN, edge cases)
tests/test_histogram.c    - Histogram unit tests (boundaries, overflow, negative)
tests/test_drop_detector.c - Drop detector unit tests (gaps, wrap-around, multi-stream)
```

## Key Design Decisions

- C11 with `_Atomic` for lock-free histogram access between capture and HTTP threads.
- No dependency on libiec61850; hand-written minimal BER parser for speed.
- BPF socket filter for EtherType 0x88BA (with/without 802.1Q VLAN).
- PHC clock auto-discovery via `ETHTOOL_GET_TS_INFO` ioctl; FD-based clockid for `clock_gettime`.
- Sparse Prometheus histogram output (skips zero-count buckets).
- Split-architecture uses length-prefixed TCP batches; collector correlates by (appID, svID, smpCnt) hash table.

## Testing

Tests use plain `assert()`. Run with `make test`. All test binaries are in `build/test_*`.

## Common CLI Flags

```
-i, --interface NAME        Network interface (default: eth0)
-p, --phc-device PATH       PHC device (auto-detected from interface)
-m, --mode direct|split     Deployment mode
-P, --prometheus-port N     Metrics port (default: 9100)
-a, --cpu-affinity N        Pin capture thread to CPU core
-s, --sched-fifo PRIO       Use SCHED_FIFO real-time scheduling
-c, --collector ADDR:PORT   Collector address for split mode
```

## License

Apache 2.0
