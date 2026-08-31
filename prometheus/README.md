# Prometheus + Grafana Setup

Quick monitoring stack to scrape `sv-subscriber` metrics and view them in Grafana.

## Usage

```bash
cd prometheus/
docker compose up -d
```

- Prometheus UI: http://localhost:19090
- Grafana: http://localhost:13000 (admin / admin)

The Grafana dashboard **SV Latencies** is auto-provisioned. No manual import needed.

## Configuration

By default, Prometheus scrapes `host.docker.internal:9100` every 5s, which targets
`sv-subscriber` running on the host with default settings.

To change the target (e.g. remote host or different port), edit `prometheus.yml`:

```yaml
static_configs:
  - targets: ["<host>:<port>"]
```

If `sv-subscriber` uses a custom port (`-P 9200`), update the target accordingly.

Grafana login can be changed via the `GF_SECURITY_ADMIN_USER` / `GF_SECURITY_ADMIN_PASSWORD`
environment variables in `docker-compose.yml`.

## Metrics

Exposed by `sv-subscriber` on `/metrics`:

| Metric                       | Type      | Description                                  |
|------------------------------|-----------|----------------------------------------------|
| `sv_capture_latency_us`      | histogram | Selected RX TS to app delivery (µs)          |
| `sv_capture_latency_us_max`  | gauge     | Maximum selected RX TS to app latency         |
| `sv_capture_latency_us_observations_total` | counter | Count per exact observed latency (µs), including values above 35000 |
| `sv_parsed_latency_us`       | histogram | Selected RX TS to post-parse (µs)             |
| `sv_hw_timestamp_to_app_latency_us` | histogram | Hardware RX TS to app PHC read (µs)      |
| `sv_sw_timestamp_to_app_latency_us` | histogram | Software RX TS to app realtime read (µs) |
| `sv_hw_to_sw_estimated_latency_us` | histogram | Estimated hardware-to-software RX latency (µs) |
| `sv_timestamp_source_frames_total` | counter | Frames by selected timestamp source       |
| `sv_sv_interval_hw_us`       | histogram | Interval between frames (selected RX timestamp, µs) |
| `sv_sv_interval_app_us`      | histogram | Interval between frames (app timestamp, µs)  |
| `sv_frames_total`            | counter   | Total SV frames received                     |
| `sv_drops_total`             | counter   | Dropped sample counts                        |
| `sv_link_up`                 | gauge     | Network link state (1=up)                    |
| `sv_kernel_oops_total`       | counter   | Kernel oops events                           |
| `sv_kernel_panic_total`      | counter   | Kernel panic events                          |
| `sv_rt_throttle_total`       | counter   | RT throttle events                           |

Histograms carry `appid` and `svid` labels and are displayed via `histogram_quantile`
(p50 / p90 / p99).

## Useful queries

- `histogram_quantile(0.99, sum by (le, appid, svid) (rate(sv_capture_latency_us_bucket[5m])))`
  -- NIC-to-application p99 latency
- `max(sv_capture_latency_us_max) / 1000`
  -- maximum exact NIC-to-application latency since each tracer started
- `histogram_quantile(1, sum by (le, appid, svid) (increase(sv_capture_latency_us_bucket[10s]))) / 1000`
  -- maximum NIC-to-application latency during the previous 10 seconds
- `sum by (latency_us) (sv_capture_latency_us_observations_total)`
  -- exact latency distribution since each tracer started
- `histogram_quantile(0.99, sum by (le, appid, svid) (rate(sv_parsed_latency_us_bucket[5m])))`
  -- NIC-to-parsed p99 latency
- `rate(sv_frames_total[5m])` -- frame rate
- `rate(sv_drops_total[5m])` -- drop rate
