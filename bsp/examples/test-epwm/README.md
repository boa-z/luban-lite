# EPWM 测试 (test-epwm)

## 一句话描述

EPWM（Enhanced PWM）增强型 PWM 测试，验证 PWM 输出与双输出（A/B）选择功能。

## 调用关系

```
MSH Shell → test-epwm → AIC EPWM 驱动 (drv/epwm)
```

## 目录结构

| 文件 | 职责 |
|------|------|
| `test_epwm.c` | EPWM 测试命令 |
| `SConscript` | 编译脚本 |

## 配置方法

| Kconfig | 说明 | 默认值 | 依赖 |
|---------|------|--------|------|
| `AIC_EPWM_DRV_TEST` | 启用 EPWM 驱动测试命令 | n | AIC_EPWM_DRV |

通过 `scons --menuconfig` 开启：

```
menuconfig
  └─ Drivers options
      └─ Drivers examples
          └─ Enable EPWM driver test command
```

## 使用说明

### `test_epwm` — EPWM 测试

**参数说明：**

| 参数 | 说明 |
|------|------|
| `<channel>` | EPWM 通道号 |
| `<period>` | PWM 周期（纳秒） |
| `<pulse>` | PWM 占空比（纳秒） |
| `<output>` | 输出选择：0 输出 A，1 输出 B，2 输出 A+B |

**使用示例：**

```bash
# 探测 EPWM 设备
test_epwm probe epwm

# EPWM1 输出 A+B，周期 1ms，占空比 50%
test_epwm set 1 1000000 500000
test_epwm enable 1

# EPWM2 只使用输出 B，周期 1ms，占空比 20%
test_epwm set_output 2 1000000 200000 1
test_epwm enable 2

# 读回通道配置
test_epwm get 1

# 关闭通道
test_epwm disable 1
```

> 只用 A+B 输出时也可以直接用 RT-Thread 自带的 pwm 命令：
> `pwm probe epwm` / `pwm set <channel> <period> <pulse>` / `pwm enable <channel>`

### `epwm_status` — 通道状态

由 EPWM 驱动导出，打印每个已使能通道的计数器模式、时基时钟、默认电平和输出动作表：

```bash
epwm_status
```

## 依赖说明

- **上游依赖**：AIC EPWM 驱动
- **下游被依赖**：无
