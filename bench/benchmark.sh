#!/usr/bin/env bash
set -euo pipefail

CORE=${CORE:-2}
RUNS=${RUNS:-5}
BIN=./bench_pool

if [[ ! -x "$BIN" ]]; then
    echo "build first: make" >&2
    exit 1
fi

gov_file="/sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor"
old_gov=""

if ! grep -q "isolcpus" /proc/cmdline; then #set in boot a isolcpus=CORE parameter to avoid outliers due to other processes running on the same core
    echo "note: isolcpus not set, occasional outliers expected"
fi


if [[ -w "$gov_file" ]]; then
    old_gov=$(cat "$gov_file")
    echo performance > "$gov_file"
    trap 'echo "$old_gov" > "$gov_file"' EXIT #old powersave governor will be restored on exit
    echo "governor: performance  "
else
    echo "warning: cannot set governor, run with sudo for stable results" >&2
    echo "current: $(cat "$gov_file" 2>/dev/null || echo unknown)"
fi

echo "core: $CORE   runs: $RUNS"
echo

for i in $(seq 1 "$RUNS"); do
    echo "--- run $i ---"
    taskset -c "$CORE" "$BIN"
done