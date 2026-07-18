# Rhealstone Benchmark — FreeRTOS on kvmm vs bare QEMU

基于 [Rhealstone](https://jacobfilipp.com/DrDobbs/articles/DDJ/1989/8902/8902a/8902a.htm)
(Dr. Dobb's, 1989) 实时性基准测试，实现 4 项核心测试 + 1 项 tick 稳定性测试。

## 测试项

| # | 名称 | 方法 | 衡量 |
|---|------|------|------|
| 1 | **Task Switch** | 两个同优先级 task 通过 `taskYIELD()` 交替 | 协作式上下文切换耗时 |
| 2 | **Preemption** | 低优先级 task 发 `xTaskNotifyGive()`，高优先级 task 立即抢占 | 抢占式调度延迟 |
| 3 | **IRQ Latency** | tick ISR 入口读 `cntvct − cntv_cval`（距 deadline 多晚） | 中断交付延迟（kvmm = VM exit + inject + re-entry） |
| 4 | **Tick Delta** | tick ISR 入口连续两次 `cntvct` 差值（与 Test 3 同时采集） | tick 间隔稳定性 |
| 5 | **Semaphore Shuffle** | 低优先级 task 释放二值信号量，高优先级 task 唤醒 | 信号量 + 调度 + 上下文切换全链路 |

每项跑 10,000 次迭代（IRQ/Tick 采集 5 秒 ≈ 500 次），报告 **avg / min / max / jitter (max−min)** 单位 ns。

## 输出示例

```
===== Rhealstone Benchmark =====
  CNTFRQ      = 62500000 Hz
  Tick rate   = 100 Hz (interval = 625000 counts = 10000000 ns)
  Iterations  = 10000

[1/4] Task Switch...
  [Task Switch] n=10000  avg=XXX  min=XXX  max=XXX  jitter=XXX ns

[2/4] Preemption...
  [Preemption] n=10000  avg=XXX  min=XXX  max=XXX  jitter=XXX ns

[3/4] IRQ Latency + Tick Jitter (5s)...
  [IRQ Latency] n=500  avg=XXX  min=XXX  max=XXX  jitter=XXX ns
  [Tick Delta] n=499  avg=XXX  min=XXX  max=XXX  jitter=XXX ns
  [Tick Delta] expected=10000000 ns

[4/4] Semaphore Shuffle...
  [Sem Shuffle] n=10000  avg=XXX  min=XXX  max=XXX  jitter=XXX ns

===== Done =====
```

## 构建

```sh
# 默认 MEM_BASE=0x70000000 (VM0 slot)
make clean && make

# 与 Linux 共存时用 VM1 slot
make clean && make MEM_BASE=0x80000000
```

`MEM_BASE` 控制 linker script 的加载地址（`MEM_BASE + 0x800000`），
必须与 kvmm boot 命令的 `@base` 参数一致。

## 运行 — kvmm

```sh
# 部署到 disk.img
debugfs -w disk.img -R "rm freertos.bin"
debugfs -w disk.img -R "write freertos.bin freertos.bin"

# 在 x-kernel guest shell 中
echo "boot /freertos.bin /freertos.dtb" > /dev/kvmm
echo "attach" > /dev/kvmm

# 如果与 Linux 同时运行 (Linux=VM0, FreeRTOS=VM1)
echo "boot /freertos.bin /freertos.dtb @0x80000000" > /dev/kvmm
```

## 运行 — bare QEMU (对照组)

```sh
# 直接用 QEMU 运行，不经过 kvmm
qemu-system-aarch64 \
    -M virt -cpu cortex-a53 -nographic -smp 1 \
    -m 256M \
    -kernel freertos.bin \
    -dtb freertos.dtb
```

**注意**：bare QEMU 需要把 linker 地址改为 QEMU virt 的默认 RAM 起始地址：

```sh
make clean && make MEM_BASE=0x40000000
```

并用匹配的 DTB（memory 节点 `reg = <0x00 0x40000000 ...>`）。

## 多轮 QEMU Benchmark

```sh
# 跑 10 轮，自动重建 freertos.qemu.bin，保存日志/CSV/图表
./bench_qemu.py --rounds 10

# 复用已有 freertos.qemu.bin，只跑测试
./bench_qemu.py --rounds 10 --skip-build
```

输出默认写入 `bench-results/<timestamp>/`：

- `logs/round-NNN.log`：每轮 QEMU 原始输出。
- `results.csv`：每轮每个指标的 `avg/min/max/jitter`。
- `summary.csv` / `summary.json`：跨轮统计均值、中位数、最小/最大和标准差。
- `benchmark-lines.png`：各指标 `avg` 和 `jitter` 随轮次变化。
- `benchmark-boxplot.png`：各指标 `avg` 和 `jitter` 分布箱线图。

脚本为 QEMU `-kernel` raw image 使用 `MEM_BASE=0x3f880000` 构建，
使链接地址 `MEM_BASE + 0x800000` 匹配 QEMU virt 的实际加载地址 `0x40080000`。

## 指标解读

### kvmm vs bare QEMU 预期差异

| 指标 | kvmm 额外开销来源 | 预期量级 |
|------|------------------|----------|
| Task Switch | 无（纯 EL1 内操作） | 接近 |
| Preemption | 无（纯 EL1 内操作） | 接近 |
| IRQ Latency | VM exit → VMM check_timer → inject PPI 27 → VM entry | **显著增大** |
| Tick Delta jitter | host timer 造成额外 VM exit，抢占 guest CPU 时间 | 增大 |
| Sem Shuffle | 无（纯 EL1 内操作） | 接近 |

**关键指标是 IRQ Latency** — 它直接反映虚拟化层的中断交付开销。
Task Switch / Preemption / Semaphore 都在 EL1 内完成（SVC 异常），
不涉及 VM exit，两种环境下应该接近。
