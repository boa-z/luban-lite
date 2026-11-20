# Luban-Lite 2.0 源码地图

> 本文件描述 **Luban-Lite 2.0** 的真实目录结构与文件位置，是 [CODEMAP.md](CODEMAP.md)（1.0 布局）的 2.0 替代版。

## 0. 与 1.0 相比的结构变化

| 主题 | 1.0 | 2.0 | 影响 |
|------|-----|-----|------|
| HAL 代码位置 | `bsp/artinchip/hal/<module>/` + `include/hal/` | **独立 Git 仓库** `bsp/artinchip/hal/{src,inc}`（扁平命名 `hal_<mod>_v<NN>.c`） | 改 HAL 需单独提交到 `luban-mini/hal` |
| HAL 头可见性 | 真实目录 `include/hal/` | `include/hal` 是指向 `hal/inc` 的 **repo linkfile（绝对软链接）** | 删除/复制时极易爆掉源仓库，见 §2 |
| HAL 版本选择 | 目录级 `hal/<mod>/v1\|v2/` | `Kconfig.chip` 的 `AIC_<MOD>_DRV_VER` → SConscript `Glob()` 精确挑文件 | 见 §7.1 |
| 内核 | baremetal / freertos / rt-thread 4.1 / ucos-ii | **仅 rt-thread 5.x** + `kernel/common` | 非 RT 内核只有应用目录，无内核仓库 |
| SoC | 10 个 | 多个（在研），对外仅 D33x | — |
| 芯片适配头 | 分散 | 新增 `sys/<soc>/include/aic_soc_hal.h`，SConscript `-include` 强制注入 | 见 §7.2 |
| 构建入口 | 同 | 同（`tools/build/` 经 linkfile 暴露到根目录） | — |

## 1. 仓库构成（repo manifest）

当前生效 manifest：`.repo/manifest.xml` → `.repo/manifests/luban-lite-2.0.xml`，共 **14 个 Git 项目**：

| 项目 | 本地路径 | 说明 |
|------|----------|------|
| `doc` | `doc/` | 文档与 AI 上下文（本文件所在仓库），并向根目录投放 linkfile |
| `application` | `application/` | 各内核应用入口 |
| `kernel/rt-thread-5.x` | `kernel/rt-thread/` | RT-Thread 5.x 内核 |
| `kernel/common` | `kernel/common/` | OSAL + 通用 shell 命令 |
| `packages/artinchip` | `packages/artinchip/` | AIC 组件包 |
| `packages/third-party` | `packages/third-party/` | 第三方组件 |
| `tools` | `tools/` | 构建脚本、Kconfig 骨架、环境工具、工具链 |
| `bsp/common` | `bsp/common/` | SDK 公共层（类型、分区、镜像、utils） |
| `bsp/examples` | `bsp/examples/` | RTOS 测试示例 |
| `bsp/examples_bare-2.0` | `bsp/examples_bare/` | 裸机测试示例 |
| `bsp/peripheral` | `bsp/peripheral/` | 板载外设驱动 |
| `bsp/artinchip-2.0` | `bsp/artinchip/` | AIC 驱动层（drv / drv_bare / sys / common / compat / SConscript） |
| `luban-mini/hal` | `bsp/artinchip/hal/` | **HAL 独立仓库**（remote 为 `luban-mini`） |
| `target-2.0` | `target/` | 板级配置 + defconfig 方案 |

## 2. 根目录软链接（linkfile）对照表

以下根目录条目**不是真实文件**，而是 `repo sync` 生成的**绝对软链接**，指向各自仓库内的真实文件。
它们不在任何仓库的工作树里，因此 `git status` 看不到；跨机器复制目录时会失效。

| 根目录条目 | 真实文件 |
|------------|----------|
| `SConstruct` / `SConscript` / `Kconfig` | `tools/build/{SConstruct,SConscript,Kconfig}` |
| `bsp/SConscript` / `bsp/Kconfig` | `tools/build/bsp/{SConscript,Kconfig}` |
| `packages/SConscript` / `packages/Kconfig` | `tools/build/packages/{SConscript,Kconfig}` |
| `win_cmd.bat` / `win_env.bat` | `tools/build/windows/` |
| `README.md` `CONTRIBUTING.md` `CODEMAP.md` `AGENTS.md` `CLAUDE.md` `.vscode` `.clang-format` `.editorconfig` | `doc/` 下同名文件（`CLAUDE.md` 与 `AGENTS.md` 同源） |
| `bsp/README-TEMPLATE.md` | `doc/README-TEMPLATE.md` |
| **`bsp/artinchip/include/hal`** | **`bsp/artinchip/hal/inc`**（由 `luban-mini/hal` 项目投放） |

> ⚠️ `bsp/artinchip/include/hal` 是绝对路径软链接。对它做 `cp -r`（解引用）或 `rm -rf <link>/xxx`
> 会读写源仓库；任何批量删除规则都必须先删除该链接本身。

## 3. 四级抽象与当前方案

```
SoC   bsp/artinchip/sys/<soc>/        芯片级：启动、中断、时钟/复位定义
 └─ Board  target/<soc>/<board>/      板级：pinmux、sys_clk、分区、镜像打包
     └─ Kernel  kernel/<os>/          内核：RT-Thread 5.x / common(OSAL)
         └─ App     application/<os>/ 应用：用户代码入口
```

**方案命名规则**（`target/configs/`）：

```
{soc}_{board}_{kernel}_{app}_defconfig
例：d33x-rt_per1-demo_rt-thread_helloworld_defconfig
```

产物目录同名：`output/{soc}_{board}_{kernel}_{app}/images/`。

当前 `target/configs/` 下的正式方案：

| defconfig | SoC | 板卡 | 内核 | 应用 |
|-----------|-----|------|------|------|
| `d33x-rt_per1-demo_rt-thread_helloworld_defconfig` | d33x-rt | per1-demo | rt-thread | helloworld |
| `d33x-rt_ver1-demo_rt-thread_helloworld_defconfig` | d33x-rt | ver1-demo | rt-thread | helloworld |

`per1-demo` 方案开启了 mailbox/rpmsg 相关 4 项：`AIC_USING_MAILBOX`、`AIC_MAILBOX_SHMEM1_ADDR`、`LPKG_USING_RPMSG_LITE`、`AIC_MAILBOX_DRV_TEST`。

> 发布包只对外提供 `per1-demo`；`ver1-demo` 是内部验证板方案，由 `sdk_for_all.json` 裁剪。

## 4. 应用层 `application/`

| 内核 | 应用 | 目录 | 关键文件 | 说明 |
|------|------|------|----------|------|
| RT-Thread | HelloWorld | `application/rt-thread/helloworld/` | `main.c` `rpmsg.c` `Kconfig` `SConscript` | 主开发入口；`rpmsg.c` 受 `AIC_MAILBOX_DRV` 守卫 |
| Baremetal | Bootloader | `application/baremetal/bootloader/` | `main.c` `cmd/` `lib/` `ldscript/` | 二级启动；另有按在研芯片命名的 `main_*.c`（不随发布包发布）；`ldscript/` 按芯片放 GCC 链接脚本 |
| Baremetal | HelloWorld | `application/baremetal/helloworld/` | `main.c` `cmd/reset.c` | 裸机示例 |
| FreeRTOS | HelloWorld | `application/freertos/helloworld/` | `main.c` `cmd/` | **无对应内核仓库**，当前不可编译 |
| uCOS-II | HelloWorld | `application/ucos-ii/helloworld/` | `main.c` | **无对应内核仓库**，当前不可编译 |

## 5. 板级配置 `target/`

```
target/
├── d33x-rt/
│   ├── common/Kconfig.board      # 板级公共 Kconfig（存储介质、串口、DDR 等）
│   ├── ver1-demo/
│   └── per1-demo/                # 板卡目录（见下表）
└── configs/                      # 方案 defconfig + outdated/ 归档
```

每个板卡目录的固定构成：

| 文件 | 作用 |
|------|------|
| `board.c` | 板级初始化入口（DDR、电源、时钟最终配置） |
| `pinmux.c` | 引脚复用表 |
| `sys_clk.c` | 系统时钟树配置 |
| `include/board.h` | 板级宏（引脚、频率、外设使能） |
| `pack/image_cfg.json` | 镜像打包配置（分区、烧录地址、启动介质） |
| `Kconfig.extend` | 板级 Kconfig 扩展点（被根 `Kconfig` source） |
| `SConscript` | 板级源码收集 |

## 6. SoC 平台层 `bsp/artinchip/sys/d33x-rt/`

| 文件 | 作用 |
|------|------|
| `startup_gcc.S` | 上电/复位入口、栈与 `.data/.bss` 搬运 |
| `vectors.S` | 中断向量表 |
| `trap_c.c` | 异常（trap）C 处理 |
| `isr.c` | 中断分发与注册 |
| `aic_drv_irq.c` | 中断控制器（PLIC/CLIC）适配 |
| `aic_clk_def.c` / `aic_reset_id.c` | 时钟/复位 ID 与默认配置 |
| `aic_gpio_id.c` | GPIO 组数量、引脚数、基址表 |
| `boot_param.c` / `boot_param_gcc.S` | 启动参数（从 bootloader 传递） |
| `boot_res_nand.c` | NAND 启动保留区处理 |
| `sys_freq.c` / `time.c` | 频率查询、delay/timer 底层 |
| `system.c` | SoC 级 system_init |
| `Kconfig.chip` | **芯片能力清单**：`AIC_CHIP_*`、`AIC_<MOD>_DRV`、`AIC_<MOD>_DRV_VER` |
| `link_script/gcc_aic.ld(.S)` | GCC 链接脚本（.S 为带预处理版本） |
| `rtconfig.py` | 该 SoC 的工具链/编译选项片段 |
| `SConscript` | SoC 源码收集 |
| `include/` | `aic_soc.h`(基址/IRQn) `aic_soc_hal.h`(HAL 适配层) `aic_arch.h` `io.h` `core_rv32.h` `cpuport.h` `csi_core.h` `csi_rv32_gcc.h` `aic_clk_id.h` `aic_dma_id.h` `aic_reset_id.h` `aic_gpio_id.h` `aic_drv_irq.h` `aic_mtop_id.h` `boot_param.h` `sys_freq.h` |

## 7. 驱动三层架构与版本选择机制

```
┌ RT-Thread Driver Framework ──── kernel/rt-thread/components/drivers/
├ DRV 层 bsp/artinchip/drv/      ── 对接驱动框架，用 OSAL，注册中断/设备
├ DRV_BARE 层 bsp/artinchip/drv_bare/ ── 裸机可用驱动（无 OS 依赖）
├ compat 层 bsp/artinchip/compat/ ── 旧版 HAL API 兼容层（SDK 侧持有，终态删除）
└ HAL 层 bsp/artinchip/hal/      ── 纯寄存器操作，独立仓库
```

### 7.1 版本选择链路（2.0 新机制）

```
sys/<soc>/Kconfig.chip            声明 AIC_<MOD>_DRV + config AIC_<MOD>_DRV_VER default "NN" if AIC_<MOD>_DRV_VNN
        │
        ▼  .config → cconfig.h
bsp/artinchip/SConscript          ver = GetConfigValue('AIC_<MOD>_DRV_VER')
        │                          LOCAL_CPPDEFINES += ['HAL_<MOD>_V<ver>_MODULE_ENABLED']
        │                          src += Glob('hal/src/hal_<mod>_v<ver>.c')
        ▼
hal/inc/hal_common.h / hal.h      按 HAL_<MOD>_V<ver>_MODULE_ENABLED 决定是否 include 对应版本头
```

- **无独立 SConscript**：`drv/`、`drv_bare/`、`hal/`、`compat/` 的源码全部由 [bsp/artinchip/SConscript](../bsp/artinchip/SConscript) 用 `Glob()` 收集（685 行）。
- 只有被 `Glob()` 命中的那个版本 `.c` 参与编译 → 同模块多版本文件可共存。
- `Kconfig.drv`（驱动级开关）与 `Kconfig.dev`（设备级：引脚/通道）放在 `drv/<mod>/` 下，由构建系统自动 source。

### 7.2 `aic_soc_hal.h` 适配层（2.0 新增，关键）

`bsp/artinchip/SConscript` 在 `AIC_CHIP_D33X_RT` 分支中对该 SoC 强制加上
`-include aic_soc_hal.h`，使 HAL 仓库**无需修改**即可在 luban-lite 环境编译。它负责：

| 适配内容 | 做法 |
|----------|------|
| 类型打通 | 依次 include `aic_common.h`(u32/container_of) `aic_io.h`(BIT/GENMASK) `core_rv32.h`(__IO) `io.h`(readl/writel) `aic_soc.h`(基址/IRQn) |
| 中断类型桥接 | `typedef IRQn_Type irq_e;`（HAL 用 `irq_e`，SDK 用 `IRQn_Type`） |
| DMA 桩 | 提供最小 `hal_dma_handle_t` 与返回 `-1` 的 `hal_dma_registger_callback()` / `hal_dma_start_it()`，让 UART DMA 分支可编译但不可用 |
| CSI 补齐 | 补 `csi_coret_get_value2()` 等 luban-lite 缺失的内联函数 |

> 新增 SoC 或启用新 HAL 模块（尤其 DMA）时，若报“类型未定义/函数冲突”，第一站就是这个文件。

### 7.3 当前启用状态（迁移进行中）

`bsp/artinchip/SConscript` 中 **242 个 `# if GetDepend(...)` 模块块处于注释状态**，
仅以下 7 个条件为活动代码：

| 行号 | 条件 | 含义 |
|------|------|------|
| 8 | `DRIVER_DRV_EN` | DRV 层总开关 |
| 10 | `DRIVER_HAL_EN` | HAL 层总开关（并 `CPPPATH += hal/inc`） |
| 19 | `AIC_CHIP_D33X_RT` | 注入 `-include aic_soc_hal.h` |
| 26 | `AIC_CMU_DRV` | 时钟（V31） |
| 34 | `AIC_GPIO_DRV` | GPIO（V21） |
| 44 | `AIC_UART_DRV` | 串口（V14） |
| 74 | `AIC_MAILBOX_DRV` | Mailbox（V11） |

即：当前 D33x 只落地 **CMU / GPIO / UART / MAILBOX** 四个驱动的编译，其余模块源码已在仓库中，
等待逐块解除注释 + 补齐对应版本 HAL。

## 8. HAL 层 `bsp/artinchip/hal/`（独立仓库）

```
bsp/artinchip/hal/
├── src/   51 个文件：hal.c, hal_<mod>_v<NN>.c, hal_rvcore_clic.c, chip_modules.json
└── inc/   54 个文件：hal.h, hal_common.h, hal_<mod>_v<NN>.h
```

| 特殊文件 | 说明 |
|----------|------|
| `src/hal.c` | HAL 通用入口（初始化、错误码、弱依赖胶水） |
| `src/hal_rvcore_clic.c` | RISC-V CLIC 中断控制器实现 |
| `inc/hal.h` | 聚合入口头：按芯片宏 `_D12X_` 等 include 芯片头（D33x 不走该机制） |
| `inc/hal_common.h` | 公共类型 `hal_status_e`(HAL_OK/ERROR/BUSY/TIMEOUT)、`HAL_MAX_DELAY`、`UNUSED`、`container_of` |
| `src/chip_modules.json` | HAL 仓库自带的“芯片 → 模块版本清单”（部分在研型号），SDK 不读取；D33x 的模块选择完全在 `bsp/artinchip/SConscript` |

> 旧 HAL API 的兼容层（原 `src/hal_compat.c`）已移出 HAL 仓库，现位于 SDK 侧 `bsp/artinchip/compat/`
> （`hal_compat.c` + `hal_compat.h`）。它是迁移脚手架：等 `drv/` 等调用者都移植到 HAL 2.0 后整目录删除。

### 8.1 模块 × 可用版本矩阵

| 模块 | `src/` 可用版本 | `inc/` | 备注 |
|------|-----------------|--------|------|
| ADCIM | v10 | v10 | — |
| Audio | v12 | v12 | I2S 编解码通路 |
| AXICFG | v11 | v11 | AXI 总线配置 |
| CAN | v10 | v10 | 经典 CAN |
| MCAN | v10 | v10 | CAN FD 控制器（`AIC_CANFD_DRV_VER` 使用） |
| CIR | — | **v11（无 .c）** | 红外，实现待补 |
| CMU | v12 / v30 / **v31** | 全部 | D33x = V31 |
| DCE | v11 | v11 | 压缩引擎 |
| DMA | v12 / v32 | v12 / v32 | — |
| Dual PSADC | v11 | v11 | 双 PSADC（`AIC_PSADC_DRV_VER` 使用） |
| GPAI(ADC) | — | **v10（无 .c）** | 实现待补 |
| GPIO | v10 / v12 / **v21** | 全部 | D33x = V21 |
| GPT | v10 | v10 + `hal_gtc_v10.h`(无 .c) | 通用定时器 |
| I2C | v10 / v20 | v10 / v20 | — |
| I2S | v10 | v10 | — |
| Mailbox | **v11** | `hal_mailbox.h`（无版本号） | D33x 已启用 |
| PBus | v20 | v20 | 外部总线 |
| PWM | v10 | v10 | — |
| QSPI | v30 / v35 | v30 / v35 | — |
| RTP | v10 | v10 | 电阻触摸 |
| SDMC | v10 | v10 | SD/eMMC |
| SID | v12 / v18 | v12 / v18 | efuse（含 DCE 用途） |
| SPI ENC | v11 / v20 | v11 / v20 | SPI 屏加密 |
| SYSCFG | v12 / v22 | v12 / v22 | — |
| THS | v11 / v21 | v11 / v21 | 温度传感器 |
| VTS | v10 | v10 | 电压传感器 |
| UART | v11 / **v14** | v11 / v14 | D33x = V14 |
| WDT | v10 / v20 | v10 / v20 | — |
| WRI | v12 / v15 | v12 / v15 | 写校验 |
| XPWM | v11 | v11 | 复合 PWM |

> 命名不对称：目录/宏用 `QSPI`、`SPIENC`、`DUAL_PSADC`、`THS`，而 `Kconfig` 侧分别是
> `AIC_QSPI_DRV_VER`、`AIC_SPIENC_DRV_VER`、`AIC_PSADC_DRV_VER`、`AIC_TSEN_DRV_VER`。

### 8.2 D33x 声明的 IP 版本与 HAL 实现缺口

`bsp/artinchip/sys/d33x-rt/Kconfig.chip` 声明的默认版本，与 `hal/src/` 是否已有对应实现：

| 模块 | 声明版本 | HAL 实现 | 状态 |
|------|----------|----------|------|
| CMU | 31 | `hal_cmu_v31.c` | ✅ 已启用 |
| GPIO | 21 | `hal_gpio_v21.c` | ✅ 已启用 |
| UART | 14 | `hal_uart_v14.c` | ✅ 已启用 |
| MAILBOX | 11 | `hal_mailbox_v11.c` | ✅ 已启用 |
| GPTIMER | 10 | `hal_gpt_v10.c` | ⏳ 有实现，SConscript 块未解注释 |
| XPWM | 11 | `hal_xpwm_v11.c` | ⏳ 同上 |
| PSADC | 11 | `hal_dual_psadc_v11.c` | ⏳ 同上 |
| I2C | 10 | `hal_i2c_v10.c` | ⏳ 同上 |
| WDT | 10 | `hal_wdt_v10.c` | ⏳ 同上 |
| SPIENC | 11 | `hal_spi_enc_v11.c` | ⏳ 同上 |
| CIR | 11 | 仅 `hal_cir_v11.h` | ❌ 缺 .c |
| GPAI | 21 | 仅 `hal_gpai_v10.h` | ❌ 缺 .c 且版本不符 |
| ADCIM | 11 | `hal_adcim_v10.c` | ❌ 版本不符（11 vs 10） |
| QSPI | 11 | `hal_qspi_v30/v35.c` | ❌ 版本不符 |
| DMA | 21 | `hal_dma_v12/v32.c` | ❌ 版本不符 |
| SYSCFG | 11 | `hal_syscfg_v12/v22.c` | ❌ 版本不符 |
| AXICFG | 10 | `hal_axicfg_v11.c` | ❌ 版本不符 |
| SDMC | 11 | `hal_sdmc_v10.c` | ❌ 版本不符 |
| SID | 11 | `hal_sid_v12/v18.c` | ❌ 版本不符 |
| WRI | 11 | `hal_wri_v12/v15.c` | ❌ 版本不符 |
| I2S | 11 | `hal_i2s_v10.c` | ❌ 版本不符 |
| AUDIO | 11 | `hal_audio_v12.c` | ❌ 版本不符 |
| RTP | 20 | `hal_rtp_v10.c` | ❌ 版本不符 |
| TSEN | 20 | `hal_ths_v11/v21.c` | ❌ 版本不符 |
| RTC / DE / GE / VE / DVP / USB / GMAC / QEP / EPWM / HRTIMER / CAP / CE / MTOP / PM | 见 Kconfig | 无同名 HAL 源文件 | ❌ 待迁移（PM 由 `drv/pm`、MTOP 由 `drv/mtop` 自带实现） |

> 该表是 2.0 迁移进度视图：解注释 `SConscript` 中某模块块之前，先确认 `hal/src/` 有
> 对应 `_v<声明版本>` 文件，否则 `Glob()` 命中为空、链接阶段报 undefined reference。


## 9. DRV 层 `bsp/artinchip/drv/`（43 个模块，下表为对外发布的 41 个）

> 另有 2 个内部在研/受限模块目录不随发布包发布，其名称被发布脚本从文档中一并剔除。

| 模块 | 源码文件 | 本地 Kconfig | 对外头文件 |
|------|----------|--------------|------------|
| adcim | `drv_adcim.c` | — | — |
| audio | `drv_audio.c` `drv_amic.c` `drv_dmic.c` | `Kconfig` | — |
| can | `drv_can.c` | — | — |
| canfd | `drv_canfd.c` | — | — |
| cap | `drv_cap.c` | `Kconfig` | — |
| ce | `drv_ce.c` | `Kconfig` | — |
| cir | `drv_cir.c` `ir_raw.c` `nec_decoder.c` `rc5_decoder.c` | `Kconfig` | `include/drv/drv_cir.h` |
| dce | `drv_dce.c` | `Kconfig` | — |
| display | `drv_de_core.c` `drv_de_ui_layer.c` `drv_de_vi_layer.c` `drv_fb.c` `drv_fb_helper.c` `drv_rgb.c` `drv_dbi.c` `drv_dsi.c` `drv_lvds.c` + `panel/` | `Kconfig` `Kconfig.rgb` `Kconfig.lvds` | `include/drv/aic_drv_de.h` |
| dma | `drv_dma.c` | — | `include/drv/drv_dma.h` |
| dvp | `v1/` `v2/`（无顶层 .c） | — | — |
| efuse | `drv_efuse.c` `efuse_cmds.c` | `Kconfig.dev` | `include/drv/drv_efuse.h` |
| epwm | `drv_epwm.c` | `Kconfig.dev` | `include/drv/drv_epwm.h` |
| ge | `aic_drv_ge.c` | `Kconfig` | `include/drv/aic_drv_ge.h` |
| gpai | `drv_gpai.c` | — | — |
| gpio | `aic_drv_gpio.c` `pinmux_utils.c` | `Kconfig.drv` `Kconfig.dev` | `include/drv/aic_drv_gpio.h` |
| gpt | `drv_gpt.c` | — | `include/drv/drv_gptimer.h` |
| hrtimer | `drv_hrtimer.c` `drv_hrtimer_pwm.c` | — | `include/drv/drv_hrtimer.h` |
| i2c | `drv_i2c.c` `drv_soft_i2c.c` | `Kconfig.drv` `Kconfig.dev` | — |
| i2s | `drv_i2s_sound.c` | — | — |
| inputcap | `drv_inputcap.c` | `Kconfig` | — |
| mac | `drv_mac.c` | — | — |
| mtop | `drv_mtop.c` | — | `include/drv/aic_drv_mtop.h` |
| pm | `drv_pm_v10..v15.c` `suspend_v10..v15.S` `pm_pin.c` `sp_ddr3_init.h` | `Kconfig` | `include/drv/pm_cfg.h` |
| psadc | `drv_psadc.c` | `Kconfig.dev` | `include/drv/drv_psadc.h` |
| pwm | `drv_pwm.c` | — | — |
| qep | `drv_qep.c` | — | — |
| qspi | `drv_qspi.c` | `Kconfig.drv` `Kconfig.dev` | `include/drv/drv_qspi.h` |
| rtc | `drv_rtc.c` | `Kconfig` | — |
| rtp | `drv_rtp.c` | — | — |
| sdmc | `drv_sdmc.c` `drv_sdcard.c` | `Kconfig` | — |
| spi | `drv_spi.c` | — | — |
| spienc | `drv_spienc.c` | `Kconfig` | `include/drv/drv_spienc.h` |
| spinand | `spinand_block.c` `spinand_parts.c` `spinand_port.c` `uffs.c` | `Kconfig.drv` `Kconfig.dev` | — |
| spinor | `spinor_sfud.c` `spinor_fal.c` | `Kconfig.drv` `Kconfig.dev` | `include/drv/fal_cfg.h` |
| syscfg | `drv_syscfg.c` | `Kconfig.dev` | — |
| tsen | `drv_tsen.c` | `Kconfig.dev` | — |
| uart | `aic_drv_uart.c` | `Kconfig.drv` `Kconfig.dev` | `include/drv/aic_drv_uart.h` |
| ve | `aic_drv_ve.c` | — | `include/drv/aic_drv_ve.h` |
| wdt | `drv_wdt.c` | — | `include/drv/aic_drv_wdt.h` |
| wri | `drv_wri.c` | — | — |
| xpwm | `drv_xpwm.c` | `Kconfig` | — |

> 命名两套并存：早期模块用 `aic_drv_<mod>.c/h`（gpio/uart/ge/ve/mtop/wdt/de），
> 新模块用 `drv_<mod>.c` / `drv_<mod>_v<NN>.c`（pm 按版本命名文件本身）。

## 10. 裸机驱动层 `bsp/artinchip/drv_bare/`（25 个模块，下表 24 个）

供 `application/baremetal/` 与 bootloader 使用，不依赖 RTOS：

| 分类 | 模块 |
|------|------|
| 控制台/输出 | `console` `printf` |
| 存储 | `mtd` `block` `sdmc` `spinor` `spinand` `qspi` `psram`(XIP) |
| 启动 | `boot_usb` `boot_usbh` |
| 基础外设 | `uart` `gpt` `rtc` `wdt` `xpwm` `inputcap` `gpai` `touch` `efuse` |
| 电源 | `pm` |
| 内存分配器 | `tlsf_heap`（`aic_tlsf.c/.h`） `umm_heap` |

## 11. 驱动头文件 `bsp/artinchip/include/`

| 目录 | 内容 |
|------|------|
| `include/drv/` | 19 个 DRV 层对外头（见 §9 表格末列），另有 `aic_drv.h`（聚合）、`pm_cfg.h`、`fal_cfg.h` |
| `include/drv_bare/` | 25 个裸机驱动头：`aic_drv_bare.h` `driver.h` `console.h` `aic_stdio.h` `block_dev.h` `mmc.h` `mtd.h` `uart.h` `wdt.h` `xpwm.h` `gpai.h` `gptimer.h` `inputcap.h` `rtc.h` `efuse.h` `ringbuffer.h` `heap.h` `tlsf.h` `aic_tlsf.h` `spinor_port.h` `spinand_port.h` `xspi_psram.h` `aic_flash_xip_def.h` |
| `include/uapi/` | 应用可见接口：`artinchip_fb.h`（帧缓冲 ioctl）、`mpp_types.h`（MPP 公共类型）、`rpmsg_aic.h`（核间通信） |
| `include/hal` | **软链接** → `bsp/artinchip/hal/inc`（见 §2） |

## 12. 公共层 `bsp/common/` 与 `bsp/artinchip/common/`

| 路径 | 内容 |
|------|------|
| `bsp/common/include/` | `aic_core.h`(u32/s32/ulong/container_of) `aic_common.h` `aic_io.h`(readl/writel/BIT) `aic_log.h`(pr_debug/pr_err) `aic_errno.h` `aic_crc32.h` `aic_image.h` `aic_partition.h` `disk_part.h` `aic_mtd_partition_parse.h` `aic_reboot_reason.h` `aic_time.h` `aic_utils.h` `aic_getopt.h` `aic_iopoll.h` `aic_list.h` `private_param.h` `itest_common.h` |
| `bsp/common/crc32/` | `crc32.c` |
| `bsp/common/image/` | `image.c`（镜像头解析/校验） |
| `bsp/common/partition/` | `partition.c` `disk_part.c`（分区表读取） |
| `bsp/common/private_param/` | `private_param.c`（私有参数区） |
| `bsp/common/utils/` | `aic_string.c` `hexdump.c` `pinmux_utils.c` `show_speed.c` `itest_common.c` |
| `bsp/artinchip/common/early/` | `early_debug.c` —— **串口 early init**，在 DDR/驱动之前把控制台打起来（分频循环按 `AIC_CHIP_*` 分支） |

## 13. 测试示例

### `bsp/examples/`（RTOS，57 个）

驱动类：`test-gpio` `test-gptimer` `test-hrtimer` `test-i2c` `test-i2c_loop` `test-uart` `test-qspi`
`test-spi*` `test-mmc` `test-sdiomgr` `test-mtd-file` `test-spinand` `test-spinor` `test-nand_verify`
`test-adc` `test-gpai` `test-psadc` `test-keyadc` `test-can` `test-canfd` `test-cap` `test-inputcap`
`test-cir` `test-qep` `test-multipwm` `test-twinkle` `test-pwm` `test-wdt` `test-rtc` `test-alarm`
`test-clock` `test-crc32` `test-ce` `test-dce` `test-dma` `test-mbox`
`test-pbus` `test-pm` `test-tsen` `test-userid` `test-rtp` `test-ctp`
多媒体：`test-audio` `test-camera` `test-csi` `test-dvp` `test-vin` `test-fb` `test-mtop`
系统类：`test-filesystem` `test-part` `test-sdiomgr` `test-dm-lib` `test-lwip` `test-mbedtls`
`test-mbedtls-csrp` `test-monkey`

### `bsp/examples_bare/`（裸机，30 个）

`test-audio` `test-can` `test-ctp` `test-dce` `test-efuse` `test-gpai` `test-gpio`
`test-gptimer` `test-hrtimer` `test-i2c` `test-inputcap` `test-keyadc` `test-mbox`
`test-mmc` `test-mtd` `test-psadc` `test-pwm` `test-rtc` `test-rtp` `test-spi`
`test-spinand` `test-spinor` `test-tsen` `test-userid` `test-ve` `test-xpwm` `test-zlib`

> 上表已剔除仅在内部维护的示例（其名称由发布脚本从文档中过滤），实际目录数见
> `ls -d bsp/examples*/test-*/`。

> 约定：每个 `test-xxx/` 以 shell 命令注册入口，配套 `Kconfig` 里的 `AIC_XXX_DEBUG` 开关。

## 14. 板载外设驱动 `bsp/peripheral/`

| 类型 | 目录 | 已支持器件 |
|------|------|------------|
| Camera | `peripheral/camera/` | `common/` bf2072, bf3a03, gc0308, gc032a, gm7150, N5, nvp6158, ov2640, ov2659, ov5640, ov7670, ov9281, sc030iot, sc031gs, sc035, tp2825b, tp9951, xs9950 |
| Touch | `peripheral/touch/` | `common/` axs15260, c123hax022, c145hax01, chsc5xxx, chsc6xxx, cst3240, cst826, ft6336, ft7411, gsl1680, gsl3676, gt911, ili2511, jd9366, rtp, st16xx, st77922, tw31xx |
| Codec | `peripheral/codec/` | alc5616, cs4344, dummy, es8311, es8388, jy6311, tlv320aic3101 |
| BT | `peripheral/bt/` | bt8858a, realtek |
| Wireless | `peripheral/wireless/` | aic8800, artinchip_general_platform, asr, atbm603x, hugeic(`hugeic_src/` + `porting/lib/`), realtek |
| RTC | `peripheral/rtc/` | pcf8563, rx8010 |
| Encoder | `peripheral/encoder/` | hec40 |
| Gyro | `peripheral/gyro/` | sc7a20h |
| SPI LCD | `peripheral/spilcd/` | `panel/` |
| SPI NAND | `peripheral/spinand/` | `inc/` + 顶层 `drv_spinand_*.c`（含 `winbond.c` 等厂商实现） |
| SPI NAND Refresh | `peripheral/spinand_refresh/` | 刷新/磨损均衡（含预编译库） |
| SPI NOR (SFUD) | `peripheral/spinor_sfud/` | `inc/` `src/` 通用 SFUD 框架 |
| NFTL | `peripheral/nftl/` | `nftl_api/` `nftl_port/`（NAND 转换层，预编译 `.a`） |

> 外设驱动位于 `bsp/peripheral/`，**允许直接调用 RT-Thread API**（OSAL 强制范围只覆盖 `bsp/artinchip/drv/`）。

## 15. 内核 `kernel/`

```
kernel/
├── common/            # OSAL + 与内核无关的 shell 命令（独立仓库）
│   ├── include/osal/  # aic_osal.h + 各内核适配头
│   ├── osal/          # aic_osal.c, aicos_memcpy.S, libaic-mem_<cpu>.a
│   ├── cmd/           # aic_cmd_base.c aic_debug_cmd.c aic_memtest_cmd.c aic_reboot_cmd.c
│   │                  # aic_axicfg_cmd.c aic_module_cmd.c(+info.h) aic_getopt.c
│   └── SConscript
└── rt-thread/         # RT-Thread 5.x（独立仓库）
    ├── src/           # 调度器、线程、ipc、timer、device model
    ├── include/       # rtthread.h, rtdef.h, rthw.h ...
    ├── libcpu/risc-v/artinchip/{c906,c907,e9xx}/   # 按 CPU 核划分
    ├── components/
    │   ├── drivers/   # serial spi i2c rtc pin pm clk dma gpio mtd block sdio
    │   │              # watchdog can hwtimer ipc mailbox net ofw phy reset sensor
    │   │              # graphic hwcrypto audio led mfd misc nvme pci scsi thermal
    │   │              # touch usb virtio wlan ata cputime ktime pic phye iio smp_call
    │   ├── finsh/     # shell / msh（提示符、命令表）
    │   ├── dfs/ fal/  # 文件系统、Flash 抽象层
    │   ├── libc/ mm/  # libc 重定向、内存管理
    │   ├── utilities/ # ulog 等
    │   ├── net/ lwp/ vbus/ legacy/ mprotect/
    ├── examples/ documentation/ tools/
    └── Kconfig SConscript
```

**CPU 核与工具链**：`libcpu/risc-v/artinchip/` 下 `c906` / `c907` / `e9xx` 三套；
`kernel/common/osal/` 的预编译库按 CPU 命名（`libaic-mem_c906fd.a` / `_e907f.a` / `_e907fdp.a`）。

## 16. OSAL（`kernel/common/include/osal/`）

| 文件 | 说明 |
|------|------|
| `aic_osal.h` | 统一入口，按内核宏分发 |
| `aic_osal_rtthread.h` | RT-Thread 适配（**当前唯一有内核仓库的适配**） |
| `aic_osal_baremetal.h` | 裸机适配 |
| `aic_osal_freertos.h` / `aic_osal_ucos_ii.h` | FreeRTOS / uCOS-II 适配（头文件保留，内核仓库未在 2.0 manifest 中） |
| `aic_osal_rhino.h` / `aic_osal_platform_yoc.h` / `aic_osal_platform_lbl.h` | 其他平台适配 |

常用接口：`aicos_msleep()` `aicos_mutex_t` `aicos_sem_t` `aicos_thread_create()` `aicos_malloc()`。

## 17. AIC 组件 `packages/artinchip/`

| 组件 | 目录 | 说明 |
|------|------|------|
| Authorization | `aic-authorization/` | 授权管理 |
| Profiler | `aic-profiler/` | 性能分析 |
| SPIENC Authorization | `aic-spienc-authorization/` | SPI 屏授权 |
| Startup UI | `aic-startup-ui/` | 开机画面 |
| Barcode | `barcode/` | 条码识别 |
| Burn-in | `burn-in/` | 老化测试 |
| Env | `env/` | 环境变量 |
| IP Manager | `ipmanager/` | IP 配置管理 |
| LVGL UI | `lvgl-ui/` | 见 §19 |
| MPP | `mpp/` | 见 §19 |
| OF（设备树） | `of/` | 设备树解析 |
| OTA | `ota/` | `ota.c` `burn.c` `flash.c` + `libfirmware_security_ota_<chip>.a`（按芯片预编译） |
| Pinmux Check | `pinmux-check/` | 引脚复用检查 |
| Thread Stack Trace | `thread_stack_trace/` | 线程栈跟踪 |
| UDS | `uds/` | UDS 诊断 |
| User ID | `userid/` | 用户 ID 区 |
| Zip FS | `zipfs/` | ZIP 只读文件系统 |

## 18. 第三方组件 `packages/third-party/`

| 分类 | 组件 |
|------|------|
| 网络 | `lwip/` `netutils/` `webclient/` `mongoose-v6.7/` `librws/` `pahomqtt/` `ptpd/` |
| 文件系统 | `dfs/` `littlefs/` `mklittlefs/` `uffs/` `levelx/` `ramdisk/` `sqlite/` |
| 多媒体/音频 | `ffmpeg/` `faad2-2.10.1/` `libFLAC/` `libape/` `libopus/` `minimp3/` `monkeys-audio-master/` `gif/` |
| 图形 | `freetype/` `awtk-ui/`（`lvgl` 在 `packages/artinchip/lvgl-ui/`） |
| 安全 | `mbedtls/` `wolfssl/` |
| USB | `cherryusb/`（v1.5.x） `cherryusb-1.0.0/`（旧版） |
| 无线/蓝牙 | `nimble/` `zephyr-bluetooth/` |
| 核间通信 | `rpmsg-lite/`（`lib/rpmsg_lite/` + `lib/include/platform/<soc>/` + `lib/rpmsg_lite/porting/platform/<soc>/`） |
| 压缩 | `miniz/` `zlib/` |
| 脚本/AI | `micropython-1.13.0/` `llm_chat/` `protobuf-c/` |
| 调试/工具 | `adbd/` `i2c-tools/` `memleak/` `memtester/` `mhz/` `cpu_load/` `cpu_usage/` `rtt_auto_exe_cmd/` `benchmark/` `bonnie/` `ota_downloader/` |
| 存储/器件 | `at24cxx/` `fdtlib/` `spi2rgb/` |
| 其他 | `cJSON-1.7.16/` `FreeRTOS-Wrapper/` `beep/` `libmodbus/` `crypto-algorithms/`(如有) |

## 19. MPP 与 LVGL UI

```
packages/artinchip/mpp/
├── base/          # 对象、buffer、线程、列表等基础设施
├── fb/            # 帧缓冲封装
├── ge/            # 图形引擎
├── ve/            # 视频编码器
├── vin/           # 视频输入（CSI/DVP/ISP），含 mpp_vin_dev_* 变体
├── middle_media/  # player / recorder 等中层组件
├── include/       # MPP 对外头
└── mpp_test/      # 功能验证用例

packages/artinchip/lvgl-ui/
├── lvgl_v8/  lvgl_v9/          # 两代 LVGL 内核（各自 canvas/fonts/porting）
├── aic_drivers/                # 显示/触摸/输入适配到 LVGL
├── aic_widgets/                # 自研控件
├── aic_demo/                   # 示例工程（含 lyrics_effect / lyrics_demo 等）
├── aic_ui.c/.h  lv_demo.c      # 组件入口与 Kconfig 控制的 demo 注册
```

## 20. 构建系统 `tools/`

| 路径 | 作用 |
|------|------|
| `tools/onestep.sh` | 开发环境入口（`source tools/onestep.sh` 后得到命令集，见 §21） |
| `tools/build/SConstruct` | SCons 主入口（经根 `SConstruct` 软链接暴露） |
| `tools/build/SConscript` | 根编译脚本，递归 `env.SConscript(dirs=...)` |
| `tools/build/Kconfig` | 根 Kconfig：芯片/板卡/内核/应用/工具链选择 |
| `tools/build/bsp/{Kconfig,SConscript}` | `bsp/` 层配置与构建（`bsp/Kconfig` 由 linkfile 提供） |
| `tools/build/packages/{Kconfig,SConscript}` | `packages/` 层配置与构建 |
| `tools/build/windows/` | Windows 命令行环境（`win_cmd.bat` `win_env.bat`） |
| `tools/scripts/aic_build.py` | 构建核心：defconfig 应用、menuconfig、`--list-def`、`--apply-def`、链接信息统计 |
| `tools/scripts/buildall.py` | `buildall` 的后端：遍历 `target/configs/*_defconfig` 全量编译并汇总 Warning |
| `tools/scripts/mk_image.py` `gen_partition_table.py` `gen_partition_file_list.py` | 镜像与分区表生成（分区表落地为根目录 `partition_table.h`） |
| `tools/scripts/elf_parse.py` `calc_linked_addr.py` `linked_size.py` | ELF 段/符号分析、容量统计 |
| `tools/scripts/release_sdk.py` | **发布包裁剪**：读取 `target/configs/sdk_for_all.json` 生成发布目录（不随发布包发布） |
| `tools/scripts/makefatfs.py` `makelittlefs.py` `makeuffs.py` `mkcpio.py` `fsinstall.py` `merge_files.py` `mk_prebootprog.py` | 文件系统镜像与资源打包 |
| `tools/scripts/` 其余 `.exe` / `.dll` / `firmware_security_encrypt` / `mkimage` | Windows 与主机侧打包工具、固件加密工具 |
| `tools/env/` | 内置 Python2.7 + pip 包 + Windows 端 Git/QEMU 等（约 233M） |
| `tools/internal/` | 内部专用脚本（发布时剔除） |
| `tools/toolchain/` | 各版本 RISC-V 工具链 tarball（`V2.6.1` / `V3.2.0-trim` / `V3.3.0-trim`，Linux + MinGW） |
| `tools/cmake/` | 主机侧 CMake |
| 根 `cconfig.h` `rtconfig.h` `partition_table.h` `.config` `.defconfig` | **生成物**：Kconfig→C 宏、RT-Thread 配置、分区表、当前配置快照 |

**工具链选择**：`tools/build/Kconfig` 中 `PRJ_RV_TOOLCHAIN_*`（默认 V2.6.1），
`PRJ_TOOLCHAIN_VER` 支持 V2.6.1 / V3.2.0 / V3.3.0；首次编译由脚本从 `tools/toolchain/`
解包到根 `toolchain/`（约 1G，`scons -c` 不清理）。

## 21. 命令速查

```bash
# 配置
scons --list-def                 # 列出所有 defconfig 方案
scons --apply-def=<name>_defconfig   # 应用方案
scons --menuconfig               # 图形化配置
scons --info                     # 查看当前方案

# 编译 / 清理
scons -j$(nproc)                 # 编译当前方案
scons -c                         # 清理中间产物（含 scons 缓存，不动 toolchain/）

# repo 提供的便捷命令（需先 source tools/onestep.sh）
cu      # 打开串口终端         cb / bm / ba / ma / me / ck / cu / buildall 等
buildall                     # 遍历 target/configs 全量编译并汇总

# 发布
python3 tools/scripts/release_sdk.py -p all
```

产物：`output/{soc}_{board}_{kernel}_{app}/images/*.img`；调试串口 115200 8N1。

## 22. 发布裁剪配置

`target/configs/sdk_for_all.json` 是发布包的唯一裁剪描述，由 `tools/scripts/release_sdk.py` 消费。
动作执行顺序固定：

```
copy → copy_file → link → reserve_file → reserve_content → delete_file → move
     → export_ver → delete_content_line → delete_content_block
     → delete_c_macro_block → delete_makefile_ifdef/ifeq → replace_content
     → rename_file → tar
```

关键约束（改配置前必读）：

| 约束 | 后果 |
|------|------|
| JSON 中 `//` 注释行**整行被丢弃**（行首 `//` 或含 `// `） | 不能在注释行尾放有效 JSON 元素 |
| `delete_content_line/block` 的 `count` **含起始匹配行** | 少写一行会留下悬空 `#endif` |
| 目标文件不存在时 `delete_line/block/macro_block` 直接 `sys.exit(110)` | 删除 SoC 后指向其路径的规则必须同步移除 |
| `delete_file` 走 `rm -rf`（sh glob，支持 `[]` 不支持 `{}`） | glob 无匹配静默通过，规则写错不会被发现 |
| `replace_content` 用 `sed 's/SRC/DST/g'` | src/dst 不能含 `/` |
| 绝对软链接必须先单独删除 | `rm -rf <link>/x`、`cp -r` 解引用都会破坏源仓库 |
| `delete_content_line` 在 `move` **之后**执行，且删除**所有**匹配行 | 根级文档要用 move 后的路径（如 `CODEMAP-v2.md`，不是 `doc/CODEMAP-v2.md`）；被过滤的文档行必须保证删完后表格/树形仍自洽 |

## 23. AI 编码注意事项（2.0 特有）

- **HAL 在独立仓库**：修改 `bsp/artinchip/hal/` 下的文件需向 `luban-mini/hal` 单独提交评审，
  与 `bsp/artinchip/` 的提交是两个 Gerrit change。
- **不要通过 `include/hal/...` 引用 HAL 头**：该路径是软链接，SCons 的 `CPPPATH` 已包含
  `hal/inc`，直接 `#include "hal_xxx_vNN.h"` 即可（`aic_soc_hal.h` 负责兜底注入）。
- **加驱动前先查版本**：见 §8.2 缺口表；若 `AIC_<MOD>_DRV_VER` 指定的版本在 `hal/src/` 不存在，
  要么补 HAL，要么改 `Kconfig.chip` 的默认版本，二选一，不要指望 `Glob()` 兜住。
- **`SConscript` 的模块块默认是注释状态**：新增驱动编译 = 解注释 + 验证版本文件存在，
  不需要新建模块级 SConscript。
- **日志宏**：ulog 用 `LOG_D/LOG_I/LOG_W/LOG_E`（需先定义 `LOG_TAG`、`LOG_LVL`）；
  HAL/aic 层用 `aic_log.h` 的 `pr_debug/pr_info/pr_warn/pr_err`。
- **非 RT-Thread 内核应用当前不可编译**：`application/{baremetal,freertos,ucos-ii}/` 仍在仓库中，
  但 2.0 manifest 只有 `kernel/rt-thread`，改动这些应用只能做静态检查。
