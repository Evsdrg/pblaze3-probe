# pblaze3-probe

与一块**已移除 NAND 闪存颗粒**的 Memblaze PBlaze3 原型卡（FPGA 主控，PCI ID
`1c5f:0530`，本机 0000:01:00.0）建立通信，并尝试获取其 SMART 等价健康信息。

协议依据：`ref/` 内 2016 年官方驱动源码（`misc-tools/pblaze3-driver/pblaze3-new`
的原样拷贝，仅作参考，不参与编译）。

## 目录结构

- `pbregs.c` → 用户态寄存器/邮箱探针工具 `pbregs`
- `pblaze_probe.c` → 内核探针模块（目标内核 7.1.7，debugfs 接口，无中断/无 DMA/无块层）
- `ref/` → 官方驱动参考代码

## 信息分层（驱动逆向结论）

| 层 | 内容 | 依赖 |
|----|------|------|
| L1 纯寄存器 | FPGA 版本、位流编译日期、固件生命周期 ST_STAGE、XADC 温度电压、PCB 温度、固件调试消息缓冲 | 仅需 FPGA 供电（已验证枚举） |
| L2 邮箱命令 | CMD_READROM(0x7) 4KB 身份键值、CMD_READMON(0x6) 健康计数（即本卡的 "SMART"） | 需卡上固件应答（无 NAND 时是否应答 = 本实验待验证项） |
| L3 存储路径 | DMA 读写 | 需 NAND，本期不做 |

## 构建

```sh
gcc -O2 -Wall -Wextra -std=gnu11 -o pbregs pbregs.c   # 用户态工具
make                                                   # 内核模块
```

## 实验步骤（需 root）

```sh
# L1：寄存器层（预期总能成功）
sudo ./pbregs report     # 身份 + 固件生命周期 + XADC 温度电压 + 板温
sudo ./pbregs dump       # BAR0 前 0x100 字节十六进制
sudo ./pbregs msg        # 固件调试消息缓冲区（0x400-0x1000）

# L2：邮箱命令层（验证固件是否活着）
sudo ./pbregs rom        # READROM：型号/序列号/固件版本/Flash 几何参数
sudo ./pbregs mon        # READMON：读写量/坏块/init 计数/过热/板温极值
sudo ./pbregs info

# 若 L2 超时，按官方驱动的初始化序列补一次 flow-control 再重试：
sudo ./pbregs flowctl
sudo ./pbregs rom && sudo ./pbregs mon

# 内核模块路径
sudo make load
cat /sys/kernel/debug/pblaze/0000:01:00.0/{stage,xadc,rom,monitor}
sudo make unload
```

`pbregs` 可用 `-t 毫秒` 调整邮箱超时（默认 200ms；官方驱动是 5 分钟死等）。

## 判读标准

- `report` 中 MB0/MB1 的 **stage** 字段：`INIT`=固件未就绪，`READY`=固件就绪待命，
  `RUN`=完全运行。无 NAND 时卡在 INIT 属预期可能结果。
- XADC 核心温度合理范围约 30~60°C；若各通道全是 0x0000/0xffff 需怀疑读序列。
- `rom`/`mon` 超时且 flowctl 后仍超时 ⇒ 结论：**固件需 NAND 存在才能运行**，
  L1 层数据即为本卡可获取的全部信息。
- `rom` 成功 ⇒ 卡的 "SMART" 路径打通，`monitor` 数据即健康报告。

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
