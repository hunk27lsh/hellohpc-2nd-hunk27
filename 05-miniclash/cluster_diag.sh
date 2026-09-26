#!/usr/bin/env bash
# Diagnosis for 05-miniclash racing efficiency.
#
# Run inside an allocation with the full 32 CPUs, e.g.:
#   salloc -p kp_run -q kp_run -c 32 -t 0:30:00
#   srun --pty bash
#   ~/hellohpc-2nd-hunk27/05-miniclash/cluster_diag.sh
#
# Answers three questions:
#   1. per-search time under full load (queue mode, one search per worker)
#   2. per-search time under light load (4 workers)
#   3. how per-task time scales with the number of racing copies (1..ncpu)
# plus cgroup quota and node load.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/source_code"

cpus=$(taskset -pc $$ | awk -F': ' '{print $2}')
cpulist=$(python3 - "$cpus" <<'PY'
import sys
out = []
for p in sys.argv[1].split(','):
    if '-' in p:
        a, b = p.split('-')
        out += list(range(int(a), int(b) + 1))
    else:
        out.append(int(p))
print(','.join(map(str, out)))
PY
)
IFS=',' read -ra CPU <<< "$cpulist"
n=${#CPU[@]}
echo "### host=$(hostname) allocated_cpus=$n list=$cpulist"
echo "### loadavg: $(cat /proc/loadavg)"
echo "### cgroup cpu.max: $(cat /sys/fs/cgroup/cpu.max 2>/dev/null || cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null || echo n/a)"
scontrol show job "${SLURM_JOB_ID:-0}" 2>/dev/null | grep -E "NumCPUs|CPU_IDs|TRES=" | head -3

[ -f cases/1/tasks.txt ] || python3 ../utils/gencase.py 32 cases/1 1001 >/dev/null
[ -f cases/small/tasks.txt ] || python3 ../utils/gencase.py 8 cases/small 777 >/dev/null

echo "### 1) full load: queue mode on 32 tasks, $n workers, per-search done_ms"
MINICLASH_MODE=queue MINICLASH_TRACE=1 ./run cases/1/tasks.txt 2>&1 | tail -n 34

echo "### 2) light load: queue mode on 8 tasks pinned to first 4 cpus"
l=$(printf '%s,' "${CPU[@]:0:4}"); l=${l%,}
MINICLASH_MODE=queue MINICLASH_TRACE=1 taskset -c "$l" ./run cases/small/tasks.txt 2>&1 | tail -n 9

echo "### 3) one task, racing with k cpus (3 seed offsets each)"
head -1 cases/1/tasks.txt > cases/one.txt
for off in 1 12345 7777777; do
	for k in 1 2 4 8 16 32; do
		[ "$k" -le "$n" ] || continue
		l=$(printf '%s,' "${CPU[@]:0:k}"); l=${l%,}
		t=$( { /usr/bin/time -f "%e" env MINICLASH_SEED=$off taskset -c "$l" ./run cases/one.txt; } 2>&1 | tail -1 )
		echo "race k=$k seed=$off wall=$t"
	done
done
echo "### diag done"
