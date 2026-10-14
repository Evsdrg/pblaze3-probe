# PBlaze3 寄存器与邮箱协议

本文整理自对 2016 年 Memblaze 官方 Linux 驱动（`colyli/misc-tools` 仓库内
`pblaze3-driver/`）的阅读，以及在本机一块 PBlaze3 卡（PCI ID `1c5f:0530`，
闪存颗粒已移除）上的实测验证。目的是让本仓库的实现可被独立理解——
`pbregs.c` 与 `pblaze_probe.c` 不含也不依赖任何第三方代码。

协议为 Memblaze 私有，非 NVMe。

## 1. PCI 标识与资源

| 项目 | 值 |
|------|-----|
| 厂商 ID / 设备 ID | `0x1c5f` / `0x0530` |
| 类别 | `0x018000` (Mass storage controller) |
| BAR0 | 64 KiB，非预取 MMIO，寄存器文件 |
| BAR2 | 64 KiB，非预取 MMIO |

设备无内核驱动绑定，`Command` 寄存器的 Memory Space Enable 位默认为 0；
访问 BAR 前需先使能内存解码（用户态可用 `setpci ... COMMAND=0002:0002`）。

## 2. BAR0 寄存器映射

| 偏移 | 名称 | 说明 |
|------|------|------|
| `0x00` | VERSION | FPGA 版本寄存器（本卡实测读 0） |
| `0x10` | REG_WRITE_MB0 | 邮箱 0 写寄存器 |
| `0x14` | REG_READ_MB0 | 邮箱 0 读（状态/回显）寄存器 |
| `0x18` | REG_WRITE_MB1 | 邮箱 1 写寄存器 |
| `0x1C` | REG_READ_MB1 | 邮箱 1 读（状态/回显）寄存器 |
| `0x20` | XADC | 片内 ADC 通道选择与结果 |
| `0x30` | FPGA_COMPILE_DATE | 位流编译日期（BCD，实测 `0x20151017`） |
| `0x34` | FPGA_COMPILE_TIME | 位流编译时间（BCD，实测 `0x00053239`） |
| `0x38` | PCB_TEMPERATURE | 板卡温度传感器（低 13 位有效） |
| `0x40` | ISRMASK | 中断掩码 |
| `0x44` | ISRDELAY | 中断延时 |
| `0x48` | ISRINFO | 中断状态信息 |
| `0x60` | FLOW_CONTROL_ADDR | 流控间接寄存器地址 |
| `0x64` | FLOW_CONTROL_DATA | 流控间接寄存器数据 |
| `0x400` – `0xFFF` | DEBUG_MSG | 固件调试消息缓冲（3072 字节） |
| `0x1000` | REG_DMA | DMA 描述符入口（本仓库未使用） |

## 3. 读邮箱状态字段（REG_READ_MBx）

| 位域 | 掩码 | 含义 |
|------|------|------|
| [15:0] | `0x0000ffff` | 数据/回显数据 |
| [23:16] | `0x00ff0000` | 回显的命令码 |
| [26:24] | `0x07000000` | 消息类型 MSG_* |
| [27:26] | `0x0c000000` | alert change |
| [29:28] | `0x30000000` | **固件生命周期阶段** |
| 30 | `0x40000000` | wait 标志 |
| 31 | `0x80000000` | interrupt 标志 |

固件阶段（ST_STAGE）取值：`0`=INIT，`1`=READY，`2`=RUN。

## 4. XADC 读取序列

XADC 提供 FPGA 片内温度与各路电压，**不经过固件，纯寄存器操作**：

1. 向 `REG_XADC` 写入 `通道号 << 16`
2. 轮询读 `REG_XADC`，直到 `(值 >> 16) & 0x7f == 通道号`（FPGA 回显通道号表示采样已锁存）
3. 读回 `值 & 0xffff` 为 16 位转换结果

换算（与官方用户态工具 `GetTemperature`/`GetVoltage` 一致）：

- 温度：`(raw >> 4) * 503.975 / 4096 - 273.15`（°C）
- 电压：`(raw >> 4) * 3.0 / 4096`（V）；VFlash IO 再乘 1.2，VFlash core 再乘 2.2

通道号：

| 通道 | 含义 | 通道 | 含义 |
|------|------|------|------|
| `0x00` | 核心温度 | `0x01` | VCCINT |
| `0x02` | VCCAUX | `0x03` | VFlash IO |
| `0x10` | VFlash core | `0x11` | VDDR3 |
| `0x20` | 核心温度最大值 | `0x21` | VCCINT 最大 |
| `0x22` | VCCAUX 最大 | `0x24` | 核心温度最小值 |
| `0x25` | VCCINT 最小 | `0x26` | VCCAUX 最小 |

板卡温度（`0x38`，低 13 位）：`raw / 16`（°C）。

## 5. 邮箱命令握手

**纯轮询，不依赖中断**。单字传输：

1. 向 `REG_WRITE_MBx` 写入 `(cmd << 16) | data`；若 `cmd` 为主命令（`0x01`–`0x20`），
   同时置中断位 `1 << 31`
2. 轮询 `REG_READ_MBx`，直到 `(值 >> 16) & 0xff == cmd`（命令码回显）
3. 回显后的 `值 & 0xffff` 即数据字段

多字命令（对应 `pblaze_comm_imm`）：主命令 → 交替用 `CMD_DATA1`/`CMD_DATA2` 逐
16 位字传输 → `CMD_ENDCMD` 结束 → 向写寄存器写 `CMD_EXITINT << 16`。
`CMD_ENDCMD` 回显的数据字段为固件状态码（见下）。

## 6. 邮箱命令码

| 命令 | 值 | 说明 |
|------|-----|------|
| `CMD_LOCKROM` | `0x02` | 锁 ROM |
| `CMD_BADTETRIS_INFO` | `0x03` | 坏块信息 |
| `CMD_READINFO` | `0x05` | 固件推送消息 |
| `CMD_READMON` | `0x06` | 健康监控（本卡的 "SMART"） |
| `CMD_READROM` | `0x07` | 4 KiB ROM 身份键值 |
| `CMD_CLEARWAIT` | `0x09` | 清 wait |
| `CMD_CLEARINT` | `0x0a` | 清中断 |
| `CMD_ENDCMD` | `0x21` | 结束命令 |
| `CMD_DATA1` | `0x22` | 数据字（奇数位） |
| `CMD_DATA2` | `0x23` | 数据字（偶数位） |
| `CMD_EXITINT` | `0x25` | 退出中断 |
| `CMD_CLEARALERT` | `0x1c` | 清 alert |

固件状态码（`CMD_ENDCMD` 回显）：`0`=成功，`1`=失败，`2`=参数无效，
`3`=设备无效，`4`=probe 未完成，`5`=内存分配失败。

## 7. 数据块格式

### 7.1 ROM（CMD_READROM，4096 字节）

NUL 结尾的键、值字符串交替排列（键、值、键、值……）。实测本卡含：
`Model:`、`Serial Number:`、`Firmware Version:`、`Firmware Size:`、
`Flash Cell:`、`Flash Type:`、`Mother Card PN:`、`FlashSectorSize:`、
`FlashPlaneSize:`、`FlashPageSize:`、`FlashBlockSize:`、`FlashLunSize:`、
`FlashChipSize:`、`MaxAddressedBlockSizeInTetris:`、`DeviceIndex:` 等。

### 7.2 健康监控（CMD_READMON）

56 字节小端（x86-64 上无填充差异）：

| 偏移 | 类型 | 字段 |
|------|------|------|
| 0 | u64 | 逻辑读计数 |
| 8 | u64 | flash 读计数 |
| 16 | u64 | 逻辑写计数 |
| 24 | u64 | flash 写计数 |
| 32 | u32 | lsa_limit_in_tetris |
| 36 | u32 | 坏块数 |
| 40 | u16 | 坏 die 数 |
| 42 | u16 | init 计数 |
| 44 | u16 | start 计数 |
| 46 | u16 | 过热标志 |
| 48 | u16 | 系统内 tetris 数 |
| 50 | s16 | 板温最大值（raw / 128） |
| 52 | s16 | 板温最小值（raw / 128） |

### 7.3 推送信息（CMD_READINFO）

`struct pblaze_print_info`：3×u16 头部（infolen/paramlen/strlen）+ 80×u32 值数组，
共 328 字节（u32 对齐使头部后有 2 字节填充）。

## 8. 实测局限

本机卡已移除闪存颗粒。实测结论：

- **L1 寄存器层与 L2 邮箱层可用**：ROM 身份信息、XADC 温度电压、
  位流日期、链路状态均正常。
- **依赖 FTL 的健康数据不可用**：`CMD_READMON` 命令完成但返回全零，
  `CMD_READINFO` 返回无效值，固件阶段停在 `INIT`——因为固件无法完成
  闪存扫描与 FTL 初始化。这与"无 NAND"一致，但**不是通信故障**。
