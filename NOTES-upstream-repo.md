# pblaze3 研究笔记（上游仓库分析）

代码来源：`../misc-tools/`（clone 自 https://github.com/colyli/misc-tools，
经本机 127.0.0.1:7897 代理拉取）。本文件分析该上游仓库的内容，
归档在工程仓库 `pblaze3-probe` 内。

## 仓库是什么

colyli（Coly Li，SUSE 内核工程师、Linux 上游 bcache 维护者，早年在 Memblaze 负责内核/驱动工作）
的个人工具箱仓库，27 个提交（2016-01 ~ 2016-07 为主），包含四块内容：

| 目录 | 内容 |
|------|------|
| `pblaze3-driver/` | **Memblaze PBlaze3 PCIe SSD 的官方驱动源码**（2016-07-11 由 Coly Li 以 `colyli@suse.de` 身份一次性提交，commit f150888） |
| `bcache_backport/` | bcache 向老内核（约 3.x）的回移补丁 + series.conf |
| `raid_backport/` `raid1_patches/` | md/raid4/5/6 性能与修复补丁的回移（70+ 个补丁） |
| `scripts/` | 补丁管理/生成脚本（gen-patches.sh、rename_patches.sh 等） |

背景推测：这是 Coly 当年为某客户项目做的"老内核 + PBlaze3 卡 + md-raid/bcache"整套存储栈的移植工作存档。

## pblaze3-driver 结构

两套驱动代码，同一硬件两代内核适配：

- `pblaze3/` — 老内核版：`bio_endio(bio, error)` 签名，版本门限低至 2.6.9/2.6.16/2.6.18（有 RHEL5 时代 shim），实际主力目标 2.6.18 ~ 4.2
- `pblaze3-new/` — 新内核版：`bio->bi_error` + `bio_endio(bio)`（内核 ≥4.3）、`make_request_fn` 返回 `blk_qc_t`（≥4.14 生效），适配 4.3+ 现代内核

每套包含：

- `driver/` — 构建出 **两个内核模块**（见 `driver/Makefile`）：
  - `memcon.ko` = pblaze_hal.o + pblaze_pcie.o + pblaze_raid.o + pblaze_con.o
  - `memdisk.ko` = pblaze_hal.o + pblaze_disk.o
- `tools/` — 用户态管理工具：memctrl（安全擦除/固件升级/LED/锁 ROM/调速）、memmonitor（ncurses 监控）、memtach、pblaze_bug_report、pblaze_smp_affinity

## 驱动架构（自底向上）

1. **pblaze_pcie.c** — PCI 驱动层，`pci_driver` 名 "memcon"。
   - **PCI ID: vendor 0x1c5f（Memblaze）/ device 0x0530**；PB_SIGNATURE 0x05300000
   - BAR0 `ioremap_nocache` 后直接读写 FPGA 寄存器：VERSION/FPGA_RELEASE/编译日期、双邮箱寄存器
     （REG_WRITE_MB0/1、REG_READ_MB0/1）、中断状态/掩码、XADC（Xilinx 7 系列 FPGA 片上 ADC，
     读 FPGA 核心温度/VCCINT/VCCAUX/DDR3 电压等，证实 FPGA 为 Xilinx）
   - 中断支持 INTx / MSI / MSI-X（per-CPU tasklet）；64-bit DMA；AER 错误处理
   - 自定义 DMA 描述符 `union pblaze_pcie_op`：cmd 型 {is_cmd, msi_vec, nr_ents, trim_idx,
     is_write, is_trim, lba} + entry 型 {sba 系统总线地址, idx}——**私有协议，非 NVMe**，
     没有 SQ/CQ doorbell 那套，而是寄存器邮箱 + DMA 描述符链 + 完成回收 FIFO
   - 邮箱命令协议（pblaze_hal.c/h）：CMD_READMON/READROM/WRITEROM/WRITEFIRMWARE/REINIT/BAECON 等
2. **pblaze_raid.c** — 驱动内部的"卡间 RAID0"聚合层。
   - 最多聚合 **2 张卡**（NR_DEVICE=2），按 GCD 生成加权条带映射表（dev_tbl/addr_tbl，含 quota），
     把多卡的 "tetris"（FPGA 内 7 个 NAND LUN 的分组单位，NR_LUN_PER_TETRIS=7，单卡最多 48 组）
     拼成一个虚拟盘；8TB 内地址映射表
   - 管理 ioctl（IOCTL_MAGIC=162）：REINITIALIZE 安全擦除、固件升级、BAECON LED、锁 ROM、监控查询
   - **DRE（数据保持增强）后台线程**：每 3 个月为周期做后台扫描/重写，对抗 NAND 电荷衰减
3. **pblaze_con.c** — 模块入口 + 自定义虚拟总线 "vbus"（bus_type + device/driver 模型），
   memcon 把虚拟设备挂到 vbus 上；`register_blkdev(0, "memcon")`
4. **pblaze_disk.c** — memdisk.ko 里的块设备层，以 `PB_SIGNATURE` 匹配 vbus 设备，为每块盘建
   gendisk（/dev/memcona、memcob…，最多 32 minor），bio 直通（无 request queue），自带调度线程、
   非 4K 对齐 bio 的拷贝回退路径

## 与"Linux 2.6 兼容"说法的核对

属实：代码中 100+ 处 `LINUX_VERSION_CODE` 条件分支，最低兼容到 2.6.9（bool/IRQF_SHARED/kmem_cache_t/
__devinit 等补齐 shim），主力区间 2.6.18（RHEL5）~ 3.x；`pblaze3-new` 适配到 4.x 现代 API。
procfs 接口（diskstat）+ 2.6 时代 bio_flags/REQ_ 宏全套分支都在。

## 代码完整性审计（2026-09-06 核查）

**结论：源码自洽完整，可独立构建**，但不含文档/固件等"厂商包"外围内容。

已验证：
- 全仓库符号交叉引用：所有被调用的内部函数均有定义（`pblaze_pcie_get_link_status`、
  `pblaze_raid_probe_one` 等仅是定义换行导致初筛误报）；无链接缺失
- 所有 `#include "..."` 本地头文件均存在；Makefile 的 objs 清单与文件一一对应
- 两个版本都含完整 5 个 .c + 4 个 .h；`pblaze3-new` 相对 `pblaze3` 仅 32 行差异，
  全部是新内核 API 适配（`#include <linux/vmalloc.h>`、`bio_endio(bio)`、
  `blk_qc_t`/`BLK_QC_T_NONE`、printf 强转），无缺文件
- 用户态工具（memctrl/memmonitor/memtach）用当前 g++ 编译通过（仅警告）
- 工具↔内核共享结构体逐字段一致：`Disk_Mon`≡`pblaze_ioctl_monitor`、
  `InitDisk_Mon`≡`pblaze_init_monitor`、`comm_status[4]`≡`NR_DEVICE*NR_CPU`；
  双方共用的 9 个 ioctl 编号/大小一致
- git 工作区与 origin/master 完全一致，仅一个分支、无 tag，没有藏起来的"另一半"

无害的历史残留（看起来像缺代码，实际是重构后没清理头文件）：
- `pblaze_hal.h` 死声明：`struct dma_addr_list`、`pb_alloc_dma_addr/pb_free_dma_addr`、
  `pblaze_get_dma_addr_slab`、`pblaze_get_ioreq_limiter`——有声明无定义无调用
- `pblaze_hal.h:824-826`：`int device_attach(struct device *dev)` 缺分号（仅在
  `< 2.6.16` 分支生效，说明该分支从未编译过，"兼容 <2.6.16"是纸面说法）
- `tools/memdef.h`：`IOCTL_DIO` 引用未定义的 `struct DirectIO`、`IOCTL_RESTART(0)`
  内核无 handler——上一代驱动遗留死宏，未被使用
- 内核有 `IOCTL_WRITEROM(10)` 而工具未封装（`pblaze_raid_send_rom` 内核路径存在）
- `pblaze3/Module.symvers` 是误提交的构建产物；两个版本均无 .ko（构建时生成）

真缺少的内容（相对完整厂商驱动包）：README/文档、固件镜像文件、udev 规则、
测试脚本、老内核源码树。固件由 memctrl -u 从用户指定目录读取，仓库本身不含。

## 实机状态（本机 01:00.0）

- `lspci -nn`：**01:00.0 Mass storage controller [0180]: Beijing Memblaze
  Technology Co. Ltd. [1c5f:0530]** —— 与驱动 ID 表完全一致
- NAND 被移除后 FPGA 仍正常枚举 → 板载 FPGA 配置位流完好，PCIe endpoint 活着
- BAR0/Region0：64KB 非 prefetch MMIO（寄存器映射与驱动定义吻合：邮箱 0x10-0x1C、
  中断 0x40-0x48、XADC 0x20、flow-control 间接寻址 0x60/0x64、调试口 0x400）
- 预期：即使装上 2.6~4.x 老内核并加载驱动，无 NAND 时 probe 会在读 ROM/tetris
  枚举阶段失败（"Read HW Rom failed!"或 tetris=0），不会出块设备；驱动不可当
  存储用，但 BAR0 寄存器层可直接与 FPGA 通信（研究私有协议的入口）

## 后续可研究方向

- 用 `pblaze3/`（老内核版）对照 2.6.32 内核源码做可编译性验证/移植
- 从寄存器与 DMA 描述符定义逆向整理 Memblaze 私有协议文档
- memmonitor/memctrl 的 ioctl 接口与内核 `pblaze_raid.c` 的对应关系
