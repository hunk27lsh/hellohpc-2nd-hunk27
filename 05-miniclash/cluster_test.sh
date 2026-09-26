#!/usr/bin/env bash
# Cluster calibration/validation for 05-miniclash.
#
# Run this inside an allocation that has 32 CPUs, e.g.:
#   salloc -p kp_run -q kp_run -c 32 -t 1:00:00
#   srun --pty bash
#   ~/miniclash/cluster_test.sh
#
# It builds, calibrates the single-core collision time, then runs and verifies
# all four official-size cases and prints wall times plus score estimates.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/source_code"

ncpu="$(nproc)"
echo "### host=$(hostname) nproc=$ncpu"
first_cpu="$(taskset -pc $$ | awk -F': ' '{print $2}' | cut -d, -f1 | cut -d- -f1)"

echo "### build"
make clean >/dev/null
make all -j"$ncpu" || exit 1

# --- single-core calibration: 8 searches, queue mode, one CPU -------------
if [ ! -f cases/small/tasks.txt ]; then
	python3 ../utils/gencase.py 8 cases/small 777 >/dev/null
fi
echo "### single-core calibration (8 queue-mode searches on cpu $first_cpu)"
start=$(date +%s.%N)
MINICLASH_MODE=queue MINICLASH_TRACE=1 taskset -c "$first_cpu" ./run cases/small/tasks.txt 2>&1 | tail -n 9
end=$(date +%s.%N)
echo "### single-core 8-task total: $(echo "$end $start" | awk '{printf "%.2f", $1-$2}') s (mean per search / 8)"
python3 ../utils/verify.py cases/small/tasks.txt

# --- official-size cases ---------------------------------------------------
for spec in "1 32 1001 4500 60000" "2 64 2002 8000 120000" "3 128 3003 15000 240000" "4 256 4004 24000 480000"; do
	set -- $spec
	id=$1 cnt=$2 seed=$3 full=$4 zero=$5
	d=cases/$id
	if [ ! -f "$d/tasks.txt" ] || [ "$(wc -l < "$d/tasks.txt")" -ne "$cnt" ]; then
		rm -rf "$d"
		python3 ../utils/gencase.py "$cnt" "$d" "$seed" >/dev/null
	fi
	echo "### case $id: $cnt tasks, all $ncpu threads (race mode)"
	start=$(date +%s.%N)
	./run "$d/tasks.txt" || echo "run exited nonzero"
	end=$(date +%s.%N)
	ms=$(echo "$end $start" | awk '{printf "%.0f", ($1-$2)*1000}')
	echo "case $id wall: ${ms} ms"
	python3 ../utils/verify.py "$d/tasks.txt"
	python3 ../utils/score_curve.py --value "$ms" --full "$full" --zero "$zero" --gamma 1.5
done
echo "### done"
