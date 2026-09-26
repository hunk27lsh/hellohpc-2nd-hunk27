#!/usr/bin/env bash
# Second-round diagnosis: why is per-task race time ~= single-search floor?
#
#   A) case1 in race mode with trace -> attempts per task (how many workers
#      actually searched each task) and per-task completion times
#   B) case1 in queue mode with 32 workers -> per-search duration under full load
#   C) case1 in queue mode pinned to 1 cpu -> per-search duration, light load
#      (same tasks, same seeds as B)
#   D) single task raced by all cpus, several seed offsets -> fresh min-of-32
#
# Run inside a 32-CPU allocation.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/source_code"

echo "### host=$(hostname) cpus=$(nproc)"

echo "### topology"
lscpu 2>/dev/null | grep -E "^CPU\(s\)|^Socket|^Core|^Thread|^Model name|NUMA node\("
firstcpu=$(taskset -pc $$ | awk -F': ' '{print $2}' | cut -d, -f1 | cut -d- -f1)
echo "thread_siblings of cpu$firstcpu: $(cat /sys/devices/system/cpu/cpu$firstcpu/topology/thread_siblings_list 2>/dev/null)"
echo "/proc/self/cgroup: $(cat /proc/self/cgroup | tr '\n' ' ')"
for f in $(cat /proc/self/cgroup | awk -F: '{print $3}' | sed 's|^|/sys/fs/cgroup|;s|$|/cpu.cfs_quota_us|}'); do
	q=$(cat "$f" 2>/dev/null) && echo "quota $f = $q"
done
for f in $(cat /proc/self/cgroup | awk -F: '{print $3}' | sed 's|^|/sys/fs/cgroup|;s|$|/cpu.cfs_period_us|}'); do
	p=$(cat "$f" 2>/dev/null) && echo "period $f = $p"
done
cat /sys/fs/cgroup/cpu.max 2>/dev/null | sed 's/^/cgroup.v2 cpu.max: /'

[ -f cases/1/tasks.txt ] || python3 ../utils/gencase.py 32 cases/1 1001 >/dev/null

echo "### A) case1 race mode, trace"
MINICLASH_TRACE=1 ./run cases/1/tasks.txt 2>&1 | tail -n 33

echo "### B) case1 queue mode, 32 workers (full load)"
MINICLASH_TRACE=1 MINICLASH_MODE=queue ./run cases/1/tasks.txt 2>&1 | tail -n 33

echo "### C) case1 queue mode, 1 cpu (light load, same tasks/seeds)"
first=$(taskset -pc $$ | awk -F': ' '{print $2}' | cut -d, -f1 | cut -d- -f1)
MINICLASH_TRACE=1 MINICLASH_MODE=queue taskset -c "$first" ./run cases/1/tasks.txt 2>&1 | tail -n 33

echo "### D) one task, all cpus racing, 5 seed offsets"
head -1 cases/1/tasks.txt > cases/one.txt
for off in 1 2 3 4 5; do
	t=$( { /usr/bin/time -f "%e" env MINICLASH_SEED=$off ./run cases/one.txt; } 2>&1 | tail -1 )
	echo "race k=32 seed=$off wall=$t"
done
echo "### diag2 done"
