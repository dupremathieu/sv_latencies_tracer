# SV Latencies Tracer

IEC 61850 Sample Values (SV) latency measurement tool for the [SEAPATH](https://lfenergy.org/projects/seapath/) virtualization platform. Captures SV Ethernet frames (EtherType 0x88BA) with kernel receive timestamps and measures timestamp-to-application delivery latency, exposing results as Prometheus metrics.

## Features

- Hardware or software timestamp capture via `AF_PACKET` + `SO_TIMESTAMPING`
- PHC (PTP Hardware Clock) auto-discovery per interface
- Minimal BER/ASN.1 SV parser (no libiec61850 dependency)
- Lock-free latency histograms (1 µs resolution, 0-35000 µs)
- Per-stream sample count drop detection with 16-bit wrap-around
- Prometheus `/metrics` endpoint (embedded HTTP server)
- System health monitoring (link state, kernel oops, RT throttling)
- Split-architecture support for virtualized deployments
- Real-time scheduling (`SCHED_FIFO`) and CPU affinity pinning
- Optional cumulative live latency histograms in the console
- Optional TSV sample output from `sv-subscriber`

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

To write one detailed TSV record per received SV frame, use `--output`:

```bash
sudo ./build/sv-subscriber -i eth0 --output /tmp/results.tsv
```

The output contains the stream identity, sample counter, timestamp source,
the RX/application/parsed timestamps, and both measured latencies. Disable
the Prometheus endpoint with `--no-prometheus` when only the file is needed.
The output option is currently available in direct mode only.

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
| `-E` | `--enable-hw-timestamps` | Configure hardware RX timestamps for all frames | disabled |
| `-v` | `--vlan-id` | VLAN ID filter | accept all |
| `-m` | `--mode` | `direct` or `split` | `direct` |
| `-c` | `--collector` | Collector `ADDR:PORT` | `127.0.0.1:9200` |
| `-P` | `--prometheus-port` | Metrics HTTP port | `9100` |
| `-N` | `--no-prometheus` | Disable the Prometheus endpoint | enabled |
| `-o` | `--output` | Write direct-mode samples as TSV | unset |
| `-H` | `--histogram-max` | Max histogram bucket (µs) | `35000` |
| `-b` | `--batch-size` | Batch size (split mode) | `256` |
| `-a` | `--cpu-affinity` | Pin capture thread to CPU | unset |
| `-s` | `--sched-fifo` | SCHED_FIFO priority | disabled |
| `-L` | `--live-histogram` | Show cumulative live histograms in direct mode | disabled |
| `-T` | `--live-threshold-us` | Count live values above this threshold (us) | `250` |
| `-w` | `--warmup-seconds` | Ignore measurements during startup | `0` |

## Live Console Histograms

Direct mode can display cumulative measurements since listening started without
adding console work to the real-time capture thread. The display refreshes once
per second, but `n`, `max`, and the threshold counter are never reset:

```bash
sudo ./build/sv-subscriber -i eth0 -a 3 -s 2 --live-histogram
```

To exclude startup transients, ignore the first second of reception:

```bash
sudo ./build/sv-subscriber -i eth0 --warmup-seconds 1 --live-histogram
```

Frames received during the warmup are not included in latency histograms,
intervals, drop counters, or direct-mode TSV output.

The background display thread remains under `SCHED_OTHER`. For each SV stream,
it shows the selected-RX-timestamp-to-application latency and the application
inter-frame interval, including min, p50, p99, max, a compact distribution, and
the number of observations above the configured threshold. Hardware, software,
and application-fallback timestamp counts are shown separately. When both RX
timestamps are delivered for a frame, it also shows hardware-to-application,
software-to-application, and estimated hardware-to-software latency.

Hardware timestamps use the interface PHC while software RX timestamps use
`CLOCK_REALTIME`, so the tracer never subtracts the two raw timestamps. It
instead estimates hardware-to-software latency as `(HW->app) - (SW->app)`,
where each elapsed duration is calculated within its own clock domain. The PHC
read happens immediately before the realtime read; the estimate can therefore
be lower by the small time between those two reads.

By default, the tracer does not call `SIOCSHWTSTAMP`: that configuration is
global to the network device and may be owned by `ptp4l` or another process. It
requests both timestamp types from its socket, uses a hardware timestamp when
the driver provides one and its PHC is available, and otherwise uses the
software receive timestamp. Check `sv_timestamp_source_frames_total` before
interpreting a latency histogram.

For controlled hardware-timestamp tests without an external `hwstamp_ctl`
command, pass `--enable-hw-timestamps`. The tracer then requests
`HWTSTAMP_FILTER_ALL` while preserving the current TX timestamp mode, and
restores the previous device configuration on normal exit. This setting is
device-global and can interfere with `ptp4l`; it is therefore disabled by
default and cannot be restored after `SIGKILL` or a process crash.

## Prometheus Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `sv_capture_latency_us` | histogram | Selected RX timestamp to application delivery (µs) |
| `sv_capture_latency_us_max` | gauge | Maximum RX-timestamp-to-application latency since start (µs) |
| `sv_capture_latency_us_observations_total` | counter | Count of each exact observed latency (µs), including values above 35000 |
| `sv_parsed_latency_us` | histogram | Selected RX timestamp to post-parse (µs) |
| `sv_hw_timestamp_to_app_latency_us` | histogram | Hardware RX timestamp to application PHC read (µs) |
| `sv_sw_timestamp_to_app_latency_us` | histogram | Software RX timestamp to application realtime read (µs) |
| `sv_hw_to_sw_estimated_latency_us` | histogram | Estimated hardware-to-software RX latency (µs) |
| `sv_timestamp_source_frames_total` | counter | Frames selected from hardware, software, or application-fallback timestamps |
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
