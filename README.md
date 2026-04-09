# SV Latencies Tracer

IEC 61850 Sample Values (SV) latency measurement tool for the [SEAPATH](https://seapath.github.io/) virtualization platform. Captures SV Ethernet frames (EtherType 0x88BA) with hardware timestamps and measures NIC-to-application delivery latency, exposing results as Prometheus metrics.

## Features

- Hardware timestamp capture via `AF_PACKET` + `SO_TIMESTAMPING`
- PHC (PTP Hardware Clock) auto-discovery per interface
- Minimal BER/ASN.1 SV parser (no libiec61850 dependency)
- Lock-free latency histograms (1 µs resolution, 0-500 µs)
- Per-stream sample count drop detection with 16-bit wrap-around
- Prometheus `/metrics` endpoint (embedded HTTP server)
- System health monitoring (link state, kernel oops, RT throttling)
- Split-architecture support for virtualized deployments
- Real-time scheduling (`SCHED_FIFO`) and CPU affinity pinning

## Building

```bash
make            # Build all binaries
make test       # Build and run unit tests
make clean      # Remove build artifacts
make install    # Install to /usr/local/bin (PREFIX= to override)
```

Requirements: Linux, GCC, pthreads. No external library dependencies.

## Deployment Scenarios

### Scenario A: Direct Mode

For bare-metal or SR-IOV passthrough where the application has direct NIC access with hardware timestamping.

```
┌──────────────────────────────┐
│         sv-subscriber        │
│  capture → parse → measure   │
│         :9100/metrics        │
└──────────┬───────────────────┘
           │ AF_PACKET + HW TS
       ┌───┴───┐
       │  NIC  │
       └───────┘
```

```bash
sudo ./build/sv-subscriber -i eth0 -P 9100
```

### Scenario B: Split Mode

For virtualized environments where the VM cannot access NIC hardware timestamps. A capture agent runs on the hypervisor, a subscriber runs in the VM, and a collector correlates both timestamp sources.

```
Hypervisor                    VM                        Collector
┌──────────────────┐   ┌──────────────────┐   ┌──────────────────────┐
│ sv-capture-agent │   │  sv-subscriber   │   │    sv-collector      │
│  HW timestamps   │──▶│  --mode split    │──▶│  correlate & measure │
│                  │   │  App timestamps  │   │    :9100/metrics     │
└──────┬───────────┘   └──────────────────┘   └──────────────────────┘
       │ AF_PACKET
   ┌───┴───┐
   │  NIC  │
   └───────┘
```

```bash
# On collector host
./build/sv-collector -P 9100

# On hypervisor
sudo ./build/sv-capture-agent -i eth0 -c collector-host:9200

# In VM
sudo ./build/sv-subscriber -i eth0 -m split -c collector-host:9200
```

## CLI Options

| Flag | Long | Description | Default |
|------|------|-------------|---------|
| `-i` | `--interface` | Network interface | `eth0` |
| `-p` | `--phc-device` | PHC device path | auto-detected |
| `-v` | `--vlan-id` | VLAN ID filter | accept all |
| `-m` | `--mode` | `direct` or `split` | `direct` |
| `-c` | `--collector` | Collector `ADDR:PORT` | `127.0.0.1:9200` |
| `-P` | `--prometheus-port` | Metrics HTTP port | `9100` |
| `-H` | `--histogram-max` | Max histogram bucket (µs) | `500` |
| `-b` | `--batch-size` | Batch size (split mode) | `256` |
| `-a` | `--cpu-affinity` | Pin capture thread to CPU | unset |
| `-s` | `--sched-fifo` | SCHED_FIFO priority | disabled |

## Prometheus Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `sv_capture_latency_us` | histogram | NIC HW timestamp to application delivery (µs) |
| `sv_parsed_latency_us` | histogram | NIC HW timestamp to post-parse (µs) |
| `sv_frames_total` | counter | Total SV frames received per stream |
| `sv_drops_total` | counter | Dropped SV frames per stream (smpCnt gaps) |
| `sv_link_up` | gauge | Network link state (1 = up) |
| `sv_kernel_oops_total` | counter | Kernel oops events detected |
| `sv_kernel_panic_total` | counter | Kernel panic events detected |
| `sv_rt_throttle_total` | counter | RT scheduling throttle events |

All per-stream metrics are labeled with `appid` and `svid`.

## Testing

```bash
make test
```

Runs unit tests for the SV parser (plain + VLAN frames, edge cases), latency histogram (boundaries, overflow, negative clamping), and drop detector (gaps, wrap-around, multi-stream).

## License

Apache 2.0
