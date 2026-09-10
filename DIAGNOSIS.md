# PBlaze3 卡片通信故障诊断记录

日期：2026-09-06　设备：0000:01:00.0　Memblaze PBlaze3（无 NAND 颗粒）

## 一、事实时间线

| 时间 | 事件 | 证据来源 |
|------|------|---------|
| 开机 0.43s | 卡正常枚举：`[1c5f:0530] type 00 class 0x018000`，BAR0 = f6f10000-64K，BAR2 = f6f00000-64K，加入 IOMMU group 13 | dmesg |
| 前一日 | `lspci` 能读配置空间（能打印 BAR 尺寸，说明配置读写均正常） | lspci |
| 今日首次 MMIO | `pbregs report` 读 BAR0：**全部寄存器 0xffffffff**（发生在任何写操作之前） | pbregs |
| 紧接着 | 配置空间读回 0xff（lspci 报 `Unknown header type 7f`） | lspci -xxx |
| 之后 | 执行 `remove` → 根端口被运行时电源管理挂起到 **D3hot** | `/sys/.../power_state` |
| 恢复尝试 | 全部无效（见下） | — |

## 二、恢复尝试清单（全部失败）

| 手段 | 结果 |
|------|------|
| 根端口 D3hot → D0（PMCSR 0x54 写 0） | ✅ 成功回到 D0，卡仍不出现 |
| 关闭运行时电源管理 `power/control=on` | ✅ 生效，排除再次挂起 |
| 二级总线复位 SBR（BridgeCtl bit6） | ❌ 无效 |
| 链路重训 Retrain Link（LnkCtl bit3） | ❌ 无效 |
| 整总线 rescan | ❌ 无效 |
| 根端口 remove + rescan | ✅ 端口自身恢复，卡仍不出现 |
| DPC（下游端口遏制） | 排除：该端口无 DPC 能力 |

## 三、当前实测硬件状态

| 项目 | 值 | 判读 |
|------|-----|------|
| Link Capabilities (0x64) | `0xf7737105` | Port#247, 32GT/s, x16 |
| Link Control (0x68) | `0x0c00` | ASPM Disabled、RCB 64B、LnkDisable- |
| **Link Status (0x6a)** | **`0x5081`** | **2.5GT/s, x8, bit12=1 → DLL Link Active** |
| LnkCtl2 Target Link Speed | 5GT/s | 端口尝试 Gen2，实际停在 Gen1 |
| 桥总线号 (0x18) | 00 / 01 / 01 | 正确 |
| 根端口电源状态 | D0（已强制） | 主机侧因素已排除 |
| AER 错误计数 | 全 0 | 无 AER 记录 |
| dmesg 该卡相关事件 | **完全为零** | 端口无 hotplug/DLLSC，链路丢失静默 |
| 配置空间读 | 0xff | 端点在线上但不应答配置请求 |
| BAR0 MMIO 读 | 0xffffffff | 同理 |

## 四、核心判断

1. **不能下"卡已损坏"的结论。** 决定性反证：链路已训练（2.5GT/s x8）且 **DLL Link Active = 1**
   —— 数据链路层正在与主机交换 DLLP，说明卡的 PCIe 硬核是活的、正在工作。
2. 真正的异常是：**端点不回应配置请求**（0xff、0xffffffff），而链路本身是通的。
   配置空间由硬核实现，链路活着却不应答配置，指向卡的端点逻辑（配置/TLP 响应路径）
   处于非正常状态。
3. **存在一个无法排除的主机侧解释**：根端口的运行时挂起（D3hot）会让下游完全不可达，
   症状与观察到的完全一致。D3hot 在 remove 之后确认存在；但**首次失败时端口是否已在
   D3hot 无法追溯**（无日志，端口无 hotplug 事件能力）。

## 五、本次诊断中的失误（更正记录）

1. **链路重训的第一次尝试是空操作**：写在 0x68（Link Status，只读寄存器），
   因此当时"重训无效"的结论不成立。正确偏移是 Link Control = 0x68（cap base 0x58 + 0x10），
   后来又误按 0x66 计算——寄存器偏移在此设备上需按 base+0x08=DeviceCtl / +0x0A=DevSta /
   +0x0C=LnkCap / +0x10=LnkCtl / +0x12=LnkSta 对齐。
2. **我自己制造了部分"设备消失"**：`remove` 触发根端口自动挂起到 D3hot。
3. **最关键的方法论漏洞**：从未在读取 BAR 之前检查端点 Command 寄存器（offset 0x04）的
   Memory Space Enable 位。`0xffffffff` 既可解释为"卡不应答"，也可解释为
   "主机未使能内存解码"——我直接采信了前者，这是不严谨的。

## 六、下一步（冷重启后必须严格按序执行）

1. 开局先 `echo on > /sys/bus/pci/devices/0000:00:01.1/power/control`，
   确认 `power_state` = D0。**先排除电源管理因素，再谈卡。**
2. 抓基线（任何 MMIO 之前）：
   - 完整配置空间 dump，重点看 Command 寄存器 0x04 的 **Mem+(bit1) / BusMaster+(bit2)**
   - Link Status：记录初始速率/宽度/DLL 状态
   - `dmesg -w` 持续观察
3. **确认 Mem Space Enable = 1** 后才做第一次 MMIO。
4. 第一次只做单次 `pbregs peek`（读 0x0），**立刻**复查配置空间与链路状态是否仍在。
   - 若一次读即导致掉线 → 结论：该卡在当前状态无法承受任何 BAR 访问（设计/供电鲁棒性问题）
   - 若存活 → 继续 `report` → `rom` / `mon`
5. 全程记录，尤其是"第一次成功读到的寄存器值"——这决定后续路线。

## 七、对项目路线的含义

- 若端点配置响应路径确实是坏的，则 **L2（邮箱命令 / SMART）不可达**，
  现实目标只能是 L1（寄存器层），且前提是 BAR 能应答。
- 即便冷重启后恢复，也应把"主机侧电源管理（根端口 D3hot）"永久纳入排查清单——
  它会独立制造出与"卡损坏"完全相同的症状。
