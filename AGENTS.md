# Benchmarking

For module performance comparisons, isolate the change being measured: build a
baseline and candidate with the same nginx and zstd versions, compiler, build
flags, and configuration. Do not compare context reuse against upstream if the
candidate also contains unrelated fixes; use the same code immediately before
and after the reuse change. Record both module revisions or diffs, nginx and
zstd versions, compiler, CPU affinity, and host limitations.

The reference context-reuse comparison used release builds of nginx 1.28.0
(`481d28cb4e04c8096b9b6134856891dc52ecc68f`) and statically linked zstd
1.5.7, built with GCC 13.3.0. Use the same versions when reproducing those
results; for other questions, select the relevant version and state it.

## Building matching binaries

Use separate, clean nginx build directories and absolute paths. For example,
set `BASELINE_MODULE` and `CANDIDATE_MODULE` to the two module source trees,
`ZSTD_SOURCE` to the zstd 1.5.7 source tree with its `lib/libzstd.a` built,
and `WORK` to a new scratch directory. Then run:

```sh
git clone --depth 1 --branch release-1.28.0 https://github.com/nginx/nginx.git "$WORK/nginx-baseline"
git clone --depth 1 --branch release-1.28.0 https://github.com/nginx/nginx.git "$WORK/nginx-candidate"
(
  cd "$WORK/nginx-baseline"
  ZSTD_INC="$ZSTD_SOURCE/lib" ZSTD_LIB="$ZSTD_SOURCE/lib" \
    ./auto/configure --add-module="$BASELINE_MODULE"
  make -j2
)
(
  cd "$WORK/nginx-candidate"
  ZSTD_INC="$ZSTD_SOURCE/lib" ZSTD_LIB="$ZSTD_SOURCE/lib" \
    ./auto/configure --add-module="$CANDIDATE_MODULE"
  make -j2
)
```

Confirm both builds link the intended zstd library, and verify versions and
configure options with `nginx -V`. Build without `--with-debug` for timing.

## Running

On Linux, use the repository harness with Python 3, Go, `taskset`, and the
zstd CLI installed:

```sh
python3 bench/compare.py \
  --baseline "$WORK/nginx-baseline/objs/nginx" \
  --candidate "$WORK/nginx-candidate/objs/nginx" \
  --zstd "$ZSTD_SOURCE/zstd" \
  --server-cpu 0 --client-cpu 1 | tee "$WORK/benchmark.jsonl"
```

Use two distinct CPUs available in the process affinity mask (`taskset -pc $$`);
override the CPU numbers if 0 and 1 are unavailable. Affinity is not exclusive
CPU reservation: avoid other load where possible and report contention or CPU
quotas. The harness stops its nginx processes on exit.

The harness runs each build in single-process mode on the server CPU with
`access_log off`, `sendfile off`, `zstd on`, `zstd_comp_level 3`,
`zstd_min_length 20`, and `zstd_buffers 2 128k`. It generates deterministic
8, 64, and 256 KiB synthetic HTML bodies. Before timing, it decompresses both
responses, checks exact body contents, and requires identical compressed bytes.
Do not silently remove the byte-identity check when comparing an optimization
that should preserve output.

The Go client runs on the other CPU with four concurrent keep-alive connections
and `Accept-Encoding: zstd`. Each sample warms up for 0.6 seconds and measures
for 4 seconds; requests in flight are drained before the sample ends. Run three
rounds per size, alternating baseline and candidate order. JSON lines include
each sample and median requests/sec, nginx CPU seconds, and nginx CPU
microseconds/request, using `/proc/<pid>/stat` and `SC_CLK_TCK`. Also report
the fixture hashes and compressed sizes so the workload can be identified.

Small changes may be noise on a shared host. Report the workload, concurrency,
versions and per-round variability; do not generalize the result to different
payloads, buffer settings, nginx versions, or production traffic without
measuring them.