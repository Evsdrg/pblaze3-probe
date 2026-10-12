# pblaze3-probe

与一块**已移除 NAND 闪存颗粒**的 Memblaze PBlaze3 原型卡（FPGA 主控，PCI ID
`1c5f:0530`，本机 0000:01:00.0）建立通信，并尝试获取其 SMART 等价健康信息。

协议依据：`ref/` 内 2016 年官方驱动源码（`misc-tools/pblaze3-driver/pblaze3-new`
的原样拷贝，仅作参考，不参与编译）。

## 当前实测状态（2026-10-09）

**通信未打通。** 本机当前为 `7.1.13-aosc-main`；设备实时配置空间全 `0xff`，
根端口处于 D0 但 `DLActive-`。正确重训练、Gen1 建链测试和一次独立总线热复位
都未恢复端点应答。本轮未访问 BAR、未发送 ROM／monitor 命令。
详见 [实测记录及历史位域勘误](RESULTS-2026-10-09.md)。

`pbregs` 已加入实时配置空间／内存解码预检和只读 `peek` 保护。
下一次须先恢复物理链路；不要直接运行下面的主动查询命令。
内核模块仅曾针对 `7.1.7` 构建，且尚未做完整运行验证，不能直接加载到当前内核。

## 目录结构

- `pbregs.c` → 用户态寄存器/邮箱探针工具 `pbregs`
- `pblaze_probe.c` → 内核探针模块（目标内核 7.1.7，debugfs 接口，无中断/无 DMA/无块层）
- `ref/` → 官方驱动参考代码

## 信息分层（驱动逆向结论）

| 层 | 内容 | 依赖 |
|----|------|------|
| L1 寄存器通道 | FPGA 版本、位流编译日期、固件生命周期 ST_STAGE、XADC 温度电压、PCB 温度、固件调试消息缓冲 | 需 PCIe 链路有效、内存解码已启用、BAR 能应答；XADC 查询需写通道选择寄存器 |
| L2 邮箱命令 | CMD_READROM(0x7) 4KB 身份键值、CMD_READMON(0x6) 健康计数（即本卡的 "SMART"） | 需卡上固件应答（无 NAND 时是否应答 = 本实验待验证项） |
| L3 存储路径 | DMA 读写 | 需 NAND，本期不做 |

## 构建

```sh
gcc -O2 -Wall -Wextra -std=gnu11 -o pbregs pbregs.c   # 用户态工具
make                                                   # 内核模块
```

## 实验步骤（需 root）

```sh
# 首先做实时 PCI 配置预检和单次只读 MMIO；失败即停止
sudo ./pbregs peek 0

# 以下仅在 peek 有效且设备仍可达时逐条执行，不作为批处理脚本
sudo ./pbregs msg        # 固件调试消息缓冲区（格式未确认）
sudo ./pbregs report     # 身份 + 固件状态 + XADC；包含通道选择写操作

# 邮箱命令仍待实测验证；失败时不要自动初始化、清状态或升级固件
sudo ./pbregs rom
sudo ./pbregs mon
```

原工具仍保留 `info` 和 `flowctl` 实验命令，但不是当前的恢复步骤。
内核模块加载、主动 flow-control 初始化和其他写操作须单独审核后再测。

`pbregs` 可用 `-t 毫秒` 调整邮箱超时（默认 200ms；官方驱动是 5 分钟死等）。

## 判读标准

- `report` 中 MB0/MB1 的 **stage** 字段：`INIT`=固件未就绪，`READY`=固件就绪待命，
  `RUN`=完全运行。无 NAND 时卡在 INIT 属预期可能结果。
- XADC 核心温度合理范围约 30~60°C；若各通道全是 0x0000/0xffff 需怀疑读序列。
- `rom`／`mon` 超时只表示该查询未完成，不能证明固件必须有 NAND 才能运行。
- `rom` 成功仅证明身份查询路径可用；`mon` 的协议完成和数据有效性须独立验证。
- 配置空间或 MMIO 全一不是合法温度／生命周期结果，不应将其解码成正常状态。

## 实测结果（待填）

| 项目 | 结果 | 备注 |
|------|------|------|
| FPGA version | | |
| 编译日期/时间 | | |
| MB0 stage | | |
| MB1 stage | | |
| XADC core temp | | |
| PCB temp | | |
| msg 缓冲区内容 | | |
| READROM | | |
| READMON | | |
| flowctl 后重试 | | |
| 内核模块 stage/xadc | | |
| 内核模块 rom/monitor | | |
