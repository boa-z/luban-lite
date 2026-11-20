# PBUS 并行总线测试 (test-pbus)

> 本目录同时放两套实现，`SConscript` 按驱动版本二选一编译：
>
> | 文件 | 对应驱动 | 适用 SDK |
> |---|---|---|
> | `test_pbus.c` | V10 直调 `hal_pbus.h` | luban-lite 1.0 |
> | `test_pbusv2.c` | 2.0 handle 驱动 `aic_drv_pbus.h` | luban-lite 2.0（`AIC_PBUS_DRV_V20`） |
>
> 判据是 Kconfig 的 `AIC_PBUS_DRV_V20`：定义了就编 `test_pbusv2.c`，否则编 `test_pbus.c`。

## 一句话描述

PBUS（Parallel Bus）并行总线测试。V20 版通过 `pbus` 设备配置总线时序并对 64 KB 外部设备地址空间进行读写。

## 调用关系

```
V20: MSH Shell → test-pbus → pbus 设备 (rt_device) → AIC PBUS 驱动 → PBUS 硬件
V10: MSH Shell → test-pbus → hal_pbus_* → AIC PBUS 硬件
```

## 目录结构

| 文件 | 职责 |
|------|------|
| `test_pbus.c` | V10 版：`test_pbus read/write` |
| `test_pbusv2.c` | V20 版：`probe/cfg/tim/.../disp 8080/color` |
| `SConscript` | 按 `AIC_PBUS_DRV_V20` 选择编译哪一个 |

## 配置方法

| Kconfig | 说明 | 默认值 | 依赖 |
|---------|------|--------|------|
| `AIC_USING_PBUS` | 使能 PBUS 设备（`Kconfig.board`，位于 CANFD 段之上） | n | — |
| `AIC_PBUS_DRV_TEST` | 启用 PBUS 驱动测试命令 | n | AIC_PBUS_DRV |
| `AIC_USING_DMA` | 使能 DMA 驱动（`disp 8080` 需要） | n | — |

设备名固定为 `pbus`（`AIC_PBUS_DEV_NAME`，定义在 `aic_drv_pbus.h`）。

通过 `scons --menuconfig` 开启：

```
menuconfig
  └─ Drivers options
    └─ Drivers examples
      └─ Enable PBUS driver test command
```

## v2 使用说明 — V20 handle 驱动（luban-lite 2.0）

### `test_pbus` — PBUS 配置与读写

先 `probe` 打开设备，再执行其余子命令。

| 子命令 | 参数 | 说明 |
|--------|------|------|
| `probe` | — | 查找并打开 `pbus` 设备，其余子命令的前置条件 |
| `cfg` | `<mode> <work> <width> <cs> <clkdiv>` | 手动下发总线配置（`tim sram` / `tim adc` 会一并下发各自的配置） |
| `tim sram` | — | 一键下发 SRAM 用例的总线配置 + 写/读时序（A/D split, CPU 直访, 16-bit, CS0, **HCLK/2**） |
| `tim adc` | — | 一键下发 AD7616 用例的总线配置 + 写/读时序（A/D share, CPU 直访, 16-bit, CS0, **HCLK/8**） |
| `tim` | `<w\|r> <sig> <pre> <pha0> <wait> <pha1>` | 单信号时序，`sig` ∈ ncs/nadv/nwe/noe/dat/adr（不动总线配置） |
| `adc` | — | AD7616 寄存器读写（**PBUS 通路测试，非 ADC 功能测试**）：寄存器 2~7 × 18 个数据值（9 位走 1/走 0）。配置+时序由 `tim adc` 一并下发 |
| `reset` | — | 软件复位模块 |
| `write` | `<offset> <val> [len]` | 写窗口，`offset` 为十六进制窗口内偏移，`len` 默认 2 |
| `read` | `<offset> <len>` | 读窗口并 hexdump |
| `test` | `[count]` | 走 1 + 走 0 写→回读自检，逐行打印 wrote/read/diff，`count` 默认 32 |
| `perf` | `[len]` | 用**当前** cfg/tim 计时读/写 `len` 字节并回读校验，`len` 默认 64 KB（整个窗口） |
| `disp 8080` | `[w] [h] [frames] [dma_first]` | DMA FIFO 写 8080 屏（ILI9488）：初始化表 + 逐行 DMA 刷四色带，默认 320×480×4，需 `AIC_USING_DMA=y` |
| `color` | `<0..4> [w] [h]` | 纯色对比：0 四色带，1 红、2 黄、3 绿、4 蓝；初始化后刷一帧，方便切换观察 |
| `status` | — | 打印 VERSION（期望 0x200）与 busy 状态 |

参数取值：

| 参数 | 取值 |
|------|------|
| `mode` | 0:MANU 1:A/D share 2:A/D split 3:I8080 4:I6800 |
| `work` | 0:CPU 直访 1:DMA 直访 2:DMA fifo 读 3:DMA fifo 写 4:CPU fifo 读 5:CPU fifo 写 |
| `width` | 0:16-bit 1:8-bit |
| `cs` | 0:CS0 1:CS1 |
| `clkdiv` | 1:HCLK/2 2:HCLK/4 3:HCLK/8 |

**使用示例：**

```bash
# 打开设备
test_pbus probe

# 一键下发 SRAM 总线配置 + 时序（A/D split, CPU 直访, 16-bit, CS0, HCLK/2）
test_pbus tim sram

# 单信号微调时序：写方向 NWE 的 pre/pha0/wait/pha1（不动总线配置）
test_pbus tim w nwe 1 6 1 1

# 需要手动覆盖总线配置时才用 cfg（例如换时钟）
test_pbus cfg 2 0 0 0 3

# 写/读
test_pbus write 0x100 0x1234
test_pbus read 0x100 16

# 自检与状态
test_pbus test 512
test_pbus status

# 读写速率（测 64 KB，默认；用当前 cfg/tim）
test_pbus perf
# 只测 4 KB
test_pbus perf 4096
```

### `perf` — 读写速率

和 `test` 一样，`perf` **不碰总线配置**：先用 `tim sram` / `tim adc`（或手动 `cfg`）把总线配好，
它只负责计时。对**外部窗口**做一次整块写、一次整块读，各计一次耗时并换算速率，最后回读校验：

```bash
test_pbus probe
test_pbus tim sram          # 配好总线（配置 + 时序），perf 只负责计时
test_pbus perf              # 默认 64 KB（整个窗口）
test_pbus perf 4096         # 想小一点就传长度
```

输出形如：

```
write: 65536 bytes in 12345 us -> 5185 KB/s
read : 65536 bytes in 23456 us -> 2728 KB/s
verify: 65536/65536 bytes ok
```

几点说明：

- **速率由当前时序决定，不是内存带宽**。16 位总线一笔只做一个字的访问（见 `hal_pbus_write()`），
  64 KB 就是 32768 笔，耗时几乎全部落在 PBUS 的 NCS/NWE/NOE/DAT 相位上。换一套 `cfg`/`tim`，这个数
  就跟着变——所以配置交给调用者，`perf` 自己不做任何假设。
- **校验用的是和 `test` 完全相同的 pattern**（`pbus_test_val()`，走 1/走 0）。它的周期是 32 个字
  （64 字节），**对 64 字节整数倍的地址回绕天生看不见**，所以 `verify` 全对只说明"数据搬过去了"，
  不等于整个窗口每个地址都独立（见下方"外部器件地址线"）。
- 缓冲用 `rt_malloc` 临时申请（默认 64 KB），跑完即释放。

### `disp 8080` — DMA 刷屏

PBUS **DMA FIFO 写**的端到端用例，按 `test_pbus_v2x_8080.c` 实现：
`DRAM --(R_DMA)--> PBUS_DATA(0x100) --> 8080 总线 --> 屏`。

需要 `CONFIG_AIC_USING_DMA=y`（由它带出 `AIC_DMA_DRV` / `AIC_DMA_DRV_V32`）。**DMA 由 PBUS 驱动统一处理**：
应用通过 `AIC_PBUS_CMD_DMA_WRITE` 只给 `addr / data_num / buf / len / flags`，驱动内部持有 DMA 通道、
做 dcache clean、下发 FIFO 阈值、编排 PBUS/DMA 启动顺序并等待完成。应用完全不感知 DMA 通道。

```bash
test_pbus probe
test_pbus disp 8080                 # 320x480, 4 帧，DMA 先启动
test_pbus disp 8080 320 4 1         # 只刷 4 行，快速验通路
test_pbus disp 8080 320 480 4 0     # dma_first=0：PBUS start 先于 DMA（参考的像素顺序）

# 纯色对比
test_pbus color 0                   # 四色带
test_pbus color 1                   # 红
test_pbus color 2                   # 黄
test_pbus color 3                   # 绿
test_pbus color 4                   # 蓝
```

| 参数 | 说明 |
|------|------|
| `w` | 宽度，偶数 2..1024，默认 320 |
| `h` | 高度 1..2048，默认 480 |
| `frames` | 帧数 1..100，默认 4 |
| `dma_first` | 1 = DMA 先启动（默认），0 = PBUS `START` 先 |
| `color` | 0 四色带；1 红、2 黄、3 绿、4 蓝（初始化后只刷一帧） |

流程：

- **初始化阶段**：应用设成 `I8080 + DMA FIFO 写 + 8-bit + HCLK/8`，按 ILI9488 初始化表逐条下发
  `DMA_WRITE`（驱动内部：`soft_reset → set_addr → set_data_num → DMA → 等完成`）。
- **像素阶段**：`16-bit + HCLK/2`，NCS 相位拉到 1000，逐行下发 `DMA_WRITE`（首行 `0x2C`，其余 `0x3C`）。
- 每帧打印行数/字节/耗时/KB/s/行率；结束打印 PBUS busy 和 `FIFO_STA`。

说明：

- 初始化表的 **CASET/RASET（`0x2A`/`0x2B`）固定为 320×480**，所以 `w`/`h` 只影响每行像素数和
  `data_num`，调试时请保持 320×480；换分辨率需要改表。
- DMA 源在 DRAM，驱动每次启动前对源区间做 `aicos_dcache_clean_range`。
- 目的端按参考用 32 位（`PBUS_DATA` 是 32 位口），IO 单次/突发由长度决定（< 8 字节用 single）。
- 参考里 `PBUS start` 与 DMA 的先后不一致（初始化阶段 DMA 先、像素阶段 PBUS 先），所以 `dma_first`
  映射到 `AIC_PBUS_DMA_F_DMA_FIRST` 可切换，上板确认哪一种稳定。
- 需要总线上有真实的 8080 屏/FPGA 才能看画面；波形上应看到首行命令 `0x2C`、后续行 `0x3C`。
- **掉电会清零**：ILI9488 的 GRAM 是易失的，断电后画面不保留、控制器也回到未初始化状态；上电后
  必须重新跑一次初始化。

### `test` — 总线自检

先写后读，逐行打印**写值 / 读值 / 差异**（`diff = wrote ^ read`），一路跑完再汇总，不会一失配就退出。

pattern 是**走 1 + 走 0**：前 16 个字依次置起一根数据线（`0x0001, 0x0002, … 0x8000`），后 16 个字再依次把它拉低（`0xFFFE, 0xFFFD, … 0x7FFF`），如此循环。这样**每根数据线都既出 1 又出 0**，能从 `diff` 直接判别故障类型：

| 写 1 读到 | 写 0 读到 | 结论 |
|-----------|-----------|------|
| 0 | 0 | 恒 0（断线 / 被拉低 / 未接） |
| 1 | 1 | 恒 1（被拉高 / 短到电源） |
| 0 | 1 | 反相 |

> 早期的写法（`i = 1, 3, 5, …, 0x3ff`）最大只到 `0x3FF`，**只用到 bit 0~9**，D10~D15 从头到尾没被驱动过——在这种 pattern 下，高位数据线断了也会报 PASS。改 pattern 就是为了消除这个盲区。

### `adc` — AD7616 并行接口读写用例

从设备是 **AD7616**（ADI，16 通道 DAS，双路 16 位同步采样 ADC），用**并行接口**挂在 PBUS 上，**固定占用偏移 0**，以 16 位**命令帧**读写内部寄存器。

> **⚠️ 这是 PBUS 通路测试，不是 ADC 功能测试。**
> 它只把 AD7616 的 6 个读/写配置寄存器当"存储单元"做写→读回环，**不涉及**启动转换（CONVST）、读转换结果、序列器、过采样或任何模拟行为。那些寄存器本质就是触发器，从"能存能取"的角度看和 SRAM 无异。
> 另外因为它**固定在偏移 0**，PBUS 的**外部地址线全程为 0**——它验证的是**帧内**的 6 位寄存器号 + 9 位数据，**不覆盖 PBUS 地址总线**（那部分要靠 `test`，需 SRAM 类器件）。
> 真正测 ADC 功能还需要 CONVST/BUSY/RESET 等信号，它们不走 16 位总线，得单独接线。

命令帧格式（规格书"寻址寄存器"节，表 20）：

```
D15    = W/R           1 = 写, 0 = 读
D14    = REGADDR[5]    1 = 选中 32 个序列器堆栈寄存器
D13:D9 = REGADDR[4:0]  寄存器地址低位
D8:D0  = DATA[8:0]     写时为数据，读时忽略
```

命令帧样例：

| 寄存器 | 写帧 | 读命令 |
|--------|------|--------|
| 2（配置寄存器） | `0x8401`（写数据 `0x001`） | `0x0400` |
| 7（输入范围B2） | `0x8EBF`（写数据 `0x0BF`） | `0x0E00` |

规格书定义的寄存器地图（全地址扫描结果与之一致）：

| 地址 | 名称 | 属性 |
|---|---|---|
| `0x00` | **读转换代码**（非寄存器） | 只读，实时 ADC 转换结果 |
| `0x01` | 保留 | — |
| `0x02` | 配置寄存器 | R/W |
| `0x03` | 通道寄存器 | R/W |
| `0x04` ~ `0x07` | 输入范围寄存器 A1/A2/B1/B2 | R/W，复位值 `0x00FF` |
| `0x08` | 状态寄存器 | 只读 |
| `0x09` ~ `0x1F` | 未定义 | — |
| `0x20` ~ `0x3F` | 32 个序列器堆栈寄存器（`D14=1` 选中） | R/W |

合计 **6 个 R/W 寄存器 + 32 个堆栈寄存器 + 1 个只读状态寄存器**，与全地址扫描得到的 38 个有效地址（2~7 与 32~63）完全吻合。

**测试内容**：只测**寄存器 2~7**（那 6 个读/写寄存器），每个寄存器依次写入 18 个不同的数据值并读回比对：

- 前 9 个是**走 1**：`0x001, 0x002, 0x004, … 0x100`
- 后 9 个是**走 0**：`0x1FE, 0x1FD, 0x1FB, … 0x0FF`
- 每轮先把同一个值写进全部 6 个寄存器，再全部读回 → 顺带验证寄存器互相独立

这样 **9 根数据线每根都出过 0 和 1**。输出一行一个数据值、一列一个寄存器，不匹配的格子带 `!`：

```
data   reg2   reg3   reg4   reg5   reg6   reg7
0x001  0x001  0x001  0x001  0x001  0x001  0x001
0x002  0x002  0x002  0x002  0x002  0x002  0x002
...
0x080  0x000! 0x080  0x080  0x080  0x080  0x080
0x100  0x100  0x100  0x100  0x100  0x100  0x100
0x1fe  0x17e! 0x1fe  0x1fe  0x1fe  0x1fe  0x1fe
...
ADC test: 106/108 ok (regs 2-7, 18 data values)
```

> **⚠️ 106/108 是正常的**：配置寄存器（`0x02`）的 **bit7 = SDEF（自检测错误标志），访问类型是"只读"**。规格书：`0` = 测试通过，AD7616 上电后成功完成自身配置。所以 `reg2` 那一列的 bit7 永远读回 0，**恰好说明器件自检通过**。
> 其它寄存器（`0x03` 的 bit7 = CHB[3]、`0x04~0x07` 的 bit7 = V3A 等）都是 R/W，因此只测 reg 3 / reg 32 时 18 行全对。

数据字段宽度：`DATA[8:0]` 共 **9 位**，其中 `bit8` 是"保留"但可读写的位（能存能读），`bit7` 在 `0x02` 上是只读的 SDEF。

与 SRAM 用例的差异：

| | SRAM 用例 | ADC 用例 |
|---|---|---|
| 总线模式 | `A/D split`（分离） | **`A/D share`（复用）** |
| 总线时钟 | 复位默认（HCLK/2） | **HCLK/8**（`cfg` 里给 `clkdiv=3`） |
| 访问地址 | 每个字不同偏移 | **固定偏移 0** |
| NCS 相位 | pha=10 / 11 | pha=**13** |
| DAT/NOE 相位 | 短 | 更长（dat wait=3、noe pha1=4） |

**一键跑：**

```bash
test_pbus probe
test_pbus adc
```

也可以手动：`tim adc`（含 share + HCLK/8 配置）→ `write 0x0 0x8401` → `write 0x0 0x0400` → `read 0x0 2`。

> 总线上需要有 AD7616（或兼容该命令帧的从设备）；没挂器件时 `adc` 会报不匹配，这不代表驱动有问题。

## v1 使用说明 — V10 直调 HAL（luban-lite 1.0）

`test_pbus.c` 直接调用 V10 的 `hal_pbus_*`（`hal_pbus.h`），没有 `pbus` 设备、也没有 DMA：

```bash
test_pbus read  <offset> <len>     # 读窗口并 hexdump
test_pbus write <offset> <data>    # 写一个 16 位字
```

`offset` 是十六进制窗口内偏移，`len` / `data` 是十进制。只依赖 `AIC_PBUS_DRV_TEST`，V20 的 `AIC_USING_PBUS` / `AIC_USING_DMA` 都不需要。

## 依赖说明

- **上游依赖**：AIC PBUS 硬件、`pbus` 设备（`AIC_USING_PBUS`）
- **下游被依赖**：无
- **注意**：
  - PBUS 的 16 bit 外部设备空间映射在 `PBUS_BASE + 0x10000`（64 KB）。
  - **外部器件的地址线可能少于 16 根**（本板器件只有 **10 根** → 1 KB）。PBUS 仍按完整偏移发地址，
    但器件丢掉高位，于是**超过 1 KB 的偏移会回绕到低 1 KB**（例如 `0x400` 与 `0x0` 是同一位置）。
    `test`/`perf` 用的 pattern 周期是 32 个字（64 字节），**对 64 字节整数倍的回绕不敏感**；要确认器件
    的真实窗口大小，用 `write` 往不同偏移写值再 `read` 比对是否互相覆盖。
  - `test` 自检、`adc` 用例都**需要有外部器件挂在总线上**，否则读回值无意义。
  - 查总线内容请用 `read`（16 位访问），不要用 shell 的 `p`——`p` 是按 32 位读的，而 PBUS 一笔只做 16 位，32 位读的高半字会读成 0，看起来像丢了一半数据。
