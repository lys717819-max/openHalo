
import subprocess
import re
import statistics
import csv
import itertools

CPU = 2
WARMUP = 3
REPEAT = 10

PSQL = [
    "sudo", "-u", "halo",
    "/opt/openhalo/bin/psql",
    "-h", "/var/run/openhalo",
    "-p", "5432",
    "-d", "halo_test",
    "-X", "-q", "-A", "-t",
    "-v", "ON_ERROR_STOP=1"
]

modes = {
    "BASELINE": """
        SET halo_pmc_list = '';
    """,
    "READ": """
        SET halo_pmc_list = 'cycles';
        SET halo_pmc_fast = off;
    """,
    "RDPMC": """
        SET halo_pmc_list = 'cycles';
        SET halo_pmc_fast = on;
    """
}

results = {mode: [] for mode in modes}
records = []

sql = """
SET max_parallel_workers_per_gather = 0;
SET enable_nestloop = off;
SET enable_mergejoin = off;
SET enable_hashjoin = on;

SELECT halo_pmc_init();
"""

query = """
EXPLAIN ANALYZE
SELECT count(*)
FROM pmc_a a
JOIN pmc_b b ON a.id = b.id;
"""

orders = list(itertools.permutations(modes))

# 生成交替执行的实验 SQL
for phase, count in [("WARMUP", WARMUP),
                     ("TEST", REPEAT)]:
    for i in range(count):
        for mode in orders[i % len(orders)]:
            sql += (
                f"\n\\echo BENCHMARK {phase} "
                f"{mode} {i + 1}\n"
            )
            sql += modes[mode]
            sql += query

# 使用同一个数据库连接
proc = subprocess.Popen(
    PSQL,
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1,
    cwd="/tmp"
)

# 获取数据库后端 PID
proc.stdin.write("SELECT pg_backend_pid();\n")
proc.stdin.flush()

pid = int(proc.stdout.readline().strip())

# 将数据库后端绑定到指定 CPU
subprocess.run(
    ["sudo", "taskset", "-pc", str(CPU), str(pid)],
    check=True
)

print(f"Backend PID: {pid}")
print(f"Selected CPU: {CPU}")

output, _ = proc.communicate(sql)

with open("pmc_benchmark_raw.txt", "w") as f:
    f.write(output)

if proc.returncode != 0:
    raise RuntimeError(output)

phase = None
mode = None
round_id = None
valid = None
failed = None

for line in output.splitlines():

    if line.startswith("BENCHMARK "):
        _, phase, mode, round_id = line.split()
        valid = None
        failed = None

    match = re.search(
        r"valid=(\d+), failed=(\d+)", line
    )

    if match:
        valid = int(match.group(1))
        failed = int(match.group(2))

    match = re.search(
        r"Execution Time:\s+([\d.]+)\s+ms", line
    )

    if match and phase is not None:
        elapsed = float(match.group(1))

        print(
            f"{phase:7s} {mode:8s} "
            f"Round {round_id:>2s}: "
            f"{elapsed:.3f} ms"
        )

        if phase == "TEST":
            results[mode].append(elapsed)

            records.append([
                mode, round_id, elapsed,
                valid, failed
            ])

# 导出实验数据
with open("pmc_benchmark_results.csv",
          "w", newline="") as f:

    writer = csv.writer(f)
    writer.writerow([
        "mode", "round", "execution_time_ms",
        "valid", "failed"
    ])
    writer.writerows(records)

# 检查实验完整性
for mode, values in results.items():
    if len(values) != REPEAT:
        raise RuntimeError(
            f"{mode}: incomplete results"
        )

# 统计实验结果
print("\n========== Benchmark Summary ==========")

for mode, values in results.items():
    print(f"\n{mode}")
    print(f"Count:  {len(values)}")
    print(f"Mean:   {statistics.mean(values):.3f} ms")
    print(f"Median: {statistics.median(values):.3f} ms")
    print(f"Min:    {min(values):.3f} ms")
    print(f"Max:    {max(values):.3f} ms")
    print(f"Std:    {statistics.stdev(values):.3f} ms")

baseline = statistics.mean(results["BASELINE"])
normal = statistics.mean(results["READ"])
fast = statistics.mean(results["RDPMC"])

normal_overhead = normal - baseline
fast_overhead = fast - baseline

print("\n========== Performance Comparison ==========")

print(f"READ overhead:  {normal_overhead:.3f} ms")
print(f"RDPMC overhead: {fast_overhead:.3f} ms")

print(f"Execution time reduction: "
      f"{(normal - fast) / normal * 100:.2f}%")

if normal_overhead > 0:
    print(f"Monitoring overhead reduction: "
          f"{(normal_overhead - fast_overhead) / normal_overhead * 100:.2f}%")

print("\nBenchmark completed.")
