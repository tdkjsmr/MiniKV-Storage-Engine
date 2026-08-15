#!/usr/bin/env sh

set -eu

benchmark_binary=${1:-./build-release/minikv_benchmark}
scratch_root=${2:-benchmark-results/matrix}
seed=5569640206804337480
operations=5000
keys=5000

if [ ! -x "${benchmark_binary}" ]; then
    echo "benchmark executable is not runnable: ${benchmark_binary}" >&2
    exit 1
fi

run() {
    workload=$1
    shift
    "${benchmark_binary}" \
        --workload "${workload}" \
        --operations "${operations}" \
        --keys "${keys}" \
        --value-size 100 \
        --memtable-bytes 65536 \
        --seed "${seed}" \
        --root "${scratch_root}" \
        --verify on \
        "$@"
}

run sequential-write --sync async --bloom on
run random-write --sync async --bloom on
run read-hit --sync async --bloom on --compact off
run read-miss --sync async --bloom on --compact off
run read-miss --sync async --bloom off --compact off
run read-miss --sync async --bloom on --compact on
run mixed-50 --sync async --bloom on --compact off
run mixed-95 --sync async --bloom on --compact off
run parallel-read --sync async --bloom on --compact off --threads 4

"${benchmark_binary}" \
    --workload sequential-write \
    --operations 500 \
    --value-size 100 \
    --memtable-bytes 65536 \
    --seed "${seed}" \
    --root "${scratch_root}" \
    --sync strict \
    --bloom on \
    --verify on
