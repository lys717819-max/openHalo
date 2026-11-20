# openHalo CPU PMC 性能监测实验

## 1. 项目简介

本实验在 openHalo 数据库内核中集成 CPU Performance Monitoring Counters（PMC），用于观察数据库执行过程中的硬件性能事件。

实验主要完成以下工作：

1. 实现 PMC 计数器初始化、打开、读取和关闭功能。
2. 支持基于 read() 系统调用的常规计数器读取。
3. 支持基于 RDPMC 指令的快速计数器读取。
4. 在 Hash 算子的哈希值计算路径中加入性能监测。
5. 通过 SQL 查询和基准测试验证功能与性能。

## 2. 主要文件

- src/backend/utils/misc/halo_pmc.c：PMC 核心实现。
- src/include/utils/halo_pmc.h：PMC 数据结构与接口声明。
- src/backend/executor/nodeHash.c：Hash 算子监测逻辑。
- src/backend/utils/misc/guc.c：PMC 配置参数。
- src/backend/utils/misc/Makefile：内核编译集成。
- contrib/halo_pmc/：PMC SQL 扩展。
- pmc_benchmark.py：性能测试脚本。
- pmc_benchmark_results.csv：性能测试结果。

## 3. 编译

本实验在 Linux 环境下进行，使用 openHalo 源码树编译。

在仓库根目录执行：

    make -j4
    make -C contrib/halo_pmc

安装步骤（需要相应系统权限）：

    sudo make install
    sudo make -C contrib/halo_pmc install

实验使用的 openHalo 安装目录为 /opt/openhalo。

## 4. 数据库扩展与初始化

连接数据库后，使用具有相应权限的角色执行：

    CREATE EXTENSION IF NOT EXISTS halo_pmc;

PMC 扩展提供以下初始化函数：

    SELECT halo_pmc_init();

注意：创建扩展并不等于完成 PMC 初始化。初始化函数需要在使用 PMC 的数据库后端会话中显式调用。

扩展 SQL 撤销了 PUBLIC 对 halo_pmc_init() 的默认执行权限，因此普通角色可能需要管理员授予执行权限。

## 5. PMC 配置

实验使用以下配置参数：

    SET halo_pmc_list = 'cycles,instructions';

使用常规读取模式：

    SET halo_pmc_fast = off;

使用 RDPMC 快速读取模式：

    SET halo_pmc_fast = on;

其中 cycles 表示 CPU 周期数，instructions 表示指令数。对应事件需要在实验数据库的 PMC 事件配置表中存在。

RDPMC 能否正常使用取决于处理器、Linux 内核、perf_event 权限和计数器映射条件。

## 6. Hash 算子测试

为方便观察 Hash 算子的 PMC 监测结果，可以关闭并行执行，并引导优化器选择 Hash Join：

    SET max_parallel_workers_per_gather = 0;
    SET enable_nestloop = off;
    SET enable_mergejoin = off;
    SET enable_hashjoin = on;

随后执行：

    EXPLAIN ANALYZE
    SELECT count(*)
    FROM pmc_a a
    JOIN pmc_b b ON a.id = b.id;

其中 pmc_a 和 pmc_b 是本实验预先创建的测试表。

## 7. 性能测试

实验使用 pmc_benchmark.py 比较三种运行模式：

- BASELINE：不启用 PMC 监测的基线。
- READ：使用常规计数器读取。
- RDPMC：使用 RDPMC 快速读取。

本次测试固定数据库后端的 CPU 亲和性，进行 3 次预热和每种模式 10 次正式测量。

| 模式 | 平均执行时间 |
| --- | ---: |
| BASELINE | 95.897 ms |
| READ | 185.630 ms |
| RDPMC | 105.461 ms |

相对于 BASELINE：

- READ 的平均额外开销为 89.734 ms。
- RDPMC 的平均额外开销为 9.564 ms。

按额外监测开销计算：

    (89.734 - 9.564) / 89.734 * 100% = 89.34%

因此，在本次测试条件下，RDPMC 相比 READ 的监测开销降低约 89.34%。

上述结果来自本次实验环境，不代表不同处理器、操作系统和数据库负载下都能获得相同的性能收益。

详细测量数据见 pmc_benchmark_results.csv。

## 8. 测试说明与限制

1. PMC 硬件事件依赖 Linux perf_event 接口及相关权限。
2. RDPMC 读取可能受到 CPU 迁移、硬件支持和内核配置影响。
3. 本次测试通过设置 CPU 亲和性提高计数器读取的稳定性。
4. 不同硬件平台的可用事件及读取性能可能不同。
5. PMC 初始化为会话级显式操作，不应视为数据库启动时自动初始化。
6. 性能测试结论仅适用于本次测量条件。

## 9. 实验结论

本实验在 openHalo 内核中实现了 CPU PMC 性能监测功能，并将其应用于 Hash 算子执行路径。

测试表明，常规 read() 方式能够读取硬件计数器，但频繁系统调用会引入较明显的执行开销。RDPMC 在本次实验环境下显著降低了监测开销，为数据库执行器的低开销硬件性能观测提供了一种实现方案。
