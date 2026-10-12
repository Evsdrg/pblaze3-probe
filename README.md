# pblaze3-probe

与一块**已移除 NAND 闪存颗粒**的 Memblaze PBlaze3 原型卡（FPGA 主控，PCI ID
`1c5f:0530`，本机 0000:01:00.0）建立通信，并尝试获取其 SMART 等价健康信息。

协议依据：`ref/` 内 2016 年官方驱动源码（`misc-tools/pblaze3-driver/pblaze3-new`
的原样拷贝，仅作参考，不参与编译）。

## 当前实测状态（2026-10-12）

**通信已打通。** 简单重启后链路恢复 5GT/s x8 / DLActive+，启用内存解码后
寄存器与邮箱命令均正常应答。`pbregs status` 可输出 nvidia-smi 风格的
身份／传感器／链路／健康四组字段。详见 [通信成功记录](RESULTS-2026-10-12.md)。

要点：无 NAND 时 L1 寄存器与 L2 邮箱（ROM 身份）可用；依赖闪存/FTL 的
运行时健康数据（READMON）因固件停在 INIT 阶段而不可用——工具如实标注
`unavailable`，不伪造零值。

## 目录结构

- `pbregs.c` → 用户态寄存器/邮箱探针工具 `pbregs`
- `pblaze_probe.c` → 内核探针模块（debugfs 接口，无中断/无 DMA/无块层）
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

## 使用步骤（需 root）

```sh
# 设备无驱动绑定，需先启用内存解码（内核不会自动做）
sudo setpci -s 0000:01:00.0 COMMAND=0002:0002

# nvidia-smi 风格汇总：身份 / 传感器 / 链路 / 健康
sudo ./pbregs status

# 单项查询
sudo ./pbregs peek 0     # 单次只读寄存器（预检失败即停止）
sudo ./pbregs report     # 寄存器明细 + XADC（含通道选择写）
sudo ./pbregs rom        # CMD_READROM 完整键值
sudo ./pbregs mon        # CMD_READMON 原始健康块
sudo ./pbregs msg        # 固件调试消息缓冲区
```

`pbregs` 可用 `-t 毫秒` 调整邮箱超时（默认 200ms；官方驱动是 5 分钟死等）。
原工具保留 `info`、`flowctl`、`dump` 实验命令。内核模块加载与主动
flow-control 初始化等写操作须单独审核后再测。

## 判读标准

- `status` 的 `FirmwareStage`：`INIT`=固件未就绪，`READY`=就绪待命，
  `RUN`=完全运行。**无 NAND 时停在 INIT**，此时 `health` 组标注 `unavailable`。
- `Status=Online` 仅当固件进入 RUN。`PCIeLink` 来自实时配置空间。
- XADC 电压应接近 FPGA 标称值（VCCINT≈1.0V、VCCAUX≈1.8V、VDDR≈1.5V）；
  数值明显偏离或全 0／全 1 说明读序列或链路有问题。
- `rom`／`mon` 超时只表示该查询未完成，不能据此断言固件必须有 NAND 才能运行。
- 配置空间或 MMIO 全一不是合法数据，工具会拒绝而非解码成正常状态。

## 实测结果（2026-10-12）

| 项目 | 结果 |
|------|------|
| FPGA 位流编译日期/时间 | 2015-10-17 05:32:39 |
| Model / SN | PB3LM150T12 / MH15227248 |
| 固件版本 | 01.06.5101RTM (2015-10-19) |
| Flash 型号 | Toshiba TH58TFT0DFKBA8J (MLC) |
| PCIe 链路 | x8 Gen2 (5GT/s), DLActive+ |
| 核心温度 / 板温 | 98.8°C / 58.2°C |
| VCCINT / VCCAUX / VDDR | 1.01V / 1.80V / 1.49V |
| 固件 stage | INIT（Offline） |
| READROM | 成功（MB1） |
| READMON | 完成但全零（固件未初始化） |
| 健康/性能数据 | unavailable（依赖 FTL，无 NAND 不产生） |
