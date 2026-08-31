#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

PROMETHEUS_URL="http://192.168.216.168:19090"
OUTPUT="latency-distribution.png"
APPID="0x4000"
SVID="svID0008"
XMAX=""

usage() {
	cat <<'EOF'
Usage: export_latency_png.sh [options]

Export the exact cumulative SV latency distribution from Prometheus as PNG.

Options:
  --prometheus URL  Prometheus base URL
  --output FILE     Output PNG path (default: latency-distribution.png)
  --appid APPID     SV appID label (default: 0x4000)
  --svid SVID       SV svID label (default: svID0008)
  --xmax US         Maximum latency on the X axis
  -h, --help        Show this help
EOF
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

while (($# > 0)); do
	case "$1" in
	--prometheus)
		(($# >= 2)) || die "missing value for --prometheus"
		PROMETHEUS_URL=$2
		shift 2
		;;
	--output)
		(($# >= 2)) || die "missing value for --output"
		OUTPUT=$2
		shift 2
		;;
	--appid)
		(($# >= 2)) || die "missing value for --appid"
		APPID=$2
		shift 2
		;;
	--svid)
		(($# >= 2)) || die "missing value for --svid"
		SVID=$2
		shift 2
		;;
	--xmax)
		(($# >= 2)) || die "missing value for --xmax"
		XMAX=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	-*)
		die "unknown option: $1"
		;;
	*)
		die "unexpected argument: $1"
		;;
	esac
done

command -v curl >/dev/null 2>&1 || die "curl is required"
command -v jq >/dev/null 2>&1 || die "jq is required"
command -v gnuplot >/dev/null 2>&1 || die "gnuplot is required"

[[ -n "$OUTPUT" ]] || die "output path must not be empty"
[[ -z "$XMAX" || "$XMAX" =~ ^[0-9]+$ ]] || die "--xmax must be a non-negative integer"

query=$(jq -nr \
	--arg appid "$APPID" \
	--arg svid "$SVID" \
	'"sum by (latency_us) (last_over_time(sv_capture_latency_us_observations_total{appid=\"\($appid)\", svid=\"\($svid)\"}[30d])) > 0"')

output_dir=$(dirname -- "$OUTPUT")
[[ -d "$output_dir" ]] || die "output directory does not exist: $output_dir"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

json="$tmpdir/query.json"
data="$tmpdir/latency.dat"

curl --fail --silent --show-error --get \
	--data-urlencode "query=$query" \
	"${PROMETHEUS_URL%/}/api/v1/query" >"$json"

jq -e '.status == "success" and .data.resultType == "vector"' "$json" \
	>/dev/null || die "Prometheus returned an invalid query response"

jq -r '
	.data.result[] |
	[.metric.latency_us, (.value[1] | tonumber)] |
	@tsv
' "$json" | sort -n -k1,1 >"$data"

if [[ ! -s "$data" ]]; then
	die "no latency observations found for appid=$APPID svid=$SVID"
fi

if [[ -n "$XMAX" ]]; then
	awk -v xmax="$XMAX" '$1 <= xmax' "$data" >"$tmpdir/filtered.dat"
	if [[ ! -s "$tmpdir/filtered.dat" ]]; then
		die "no latency observations found up to ${XMAX} us"
	fi
	data="$tmpdir/filtered.dat"
fi

if [[ -n "$XMAX" ]]; then
	xrange="[0:${XMAX}]"
else
	xrange="[*:*]"
fi

gnuplot <<EOF
set terminal pngcairo size 1600,900 enhanced font 'DejaVu Sans,10'
set output '${OUTPUT//\'/\\\'}'
set title 'SV capture latency distribution'
set xlabel 'Latency (microseconds)'
set ylabel 'Observation count'
set xrange $xrange
set yrange [0.8:*]
set logscale y
set grid xtics ytics
set arrow 1 from 250, graph 0 to 250, graph 1 nohead front \
    lc rgb '#cc0000' lw 2 dashtype 2
set boxwidth 1 absolute
set style fill solid 0.75 border lc rgb '#245a9b'
set key off
plot '$data' using 1:2 with boxes
EOF

printf 'Wrote %s (%s, appid=%s, svid=%s)\n' "$OUTPUT" "$query" "$APPID" "$SVID"
