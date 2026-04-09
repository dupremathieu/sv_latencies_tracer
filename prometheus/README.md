# Prometheus Setup

Quick Prometheus setup to scrape `sv-subscriber` metrics.

## Usage

```bash
cd prometheus/
docker compose up -d
```

Prometheus UI: http://localhost:19090

## Configuration

By default, Prometheus scrapes `host.docker.internal:9100` every 5s, which targets
`sv-subscriber` running on the host with default settings.

To change the target (e.g. remote host or different port), edit `prometheus.yml`:

```yaml
static_configs:
  - targets: ["<host>:<port>"]
```

If `sv-subscriber` uses a custom port (`-P 9200`), update the target accordingly.

## Useful queries

- `sv_nic_to_app_latency_us_bucket` -- NIC-to-application latency histogram
- `sv_nic_to_parsed_latency_us_bucket` -- NIC-to-parsed latency histogram
- `sv_frames_total` -- total frames received
- `sv_drops_total` -- dropped sample counts
