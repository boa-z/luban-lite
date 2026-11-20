# 输入捕获测试 (test-cap)

## 一句话描述

输入捕获（PWMCS CAP）功能测试：测量输入信号的频率与占空比，验证 CAP 驱动（hal_cap_v23）与 RT-Thread input capture 框架的链路。

## 调用关系

```
MSH Shell → test-cap → AIC CAP 驱动
```

## 目录结构

| 文件 | 职责 |
|------|------|
| `test_cap.c` | 捕获定时器测试主程序 |
| `SConscript` | 编译脚本 |

## 配置方法

| Kconfig | 说明 | 默认值 | 依赖 |
|---------|------|--------|------|
| `AIC_CAP_DRV_TEST` | 启用 CAP 驱动测试命令 | n | AIC_CAP_DRV |

通过 `scons --menuconfig` 开启：

```
menuconfig
  └─ Drivers options
      └─ Drivers examples
          └─ Enable CAP driver test command
```

## 使用说明

> 前提：被测通道需在板级 Kconfig（`AIC_USING_CAP0`…）使能并注册为 `capN` 设备，
> 通道输入引脚接被测信号（引脚复用由板级 pinmux 配置）。

### `test_cap` — 输入捕获测试

对指定通道进行输入捕获，测量信号频率和占空比。命令会启动一个后台线程消费捕获
数据并打印，到达测试时长后自动关闭通道退出。

**参数说明：**

| 参数 | 说明 |
|------|------|
| `channel` | 捕获通道号 |
| `[second]` | 测试时长（秒，默认 10） |

**使用示例：**

```bash
# 测试通道 0，默认 10 秒
test_cap 0

# 测试通道 1，持续 5 秒
test_cap 1 5
```


## 依赖说明

- **上游依赖**：AIC CAP 驱动（`AIC_CAP_DRV`）、RT-Thread Finsh（`RT_USING_FINSH`）
- **下游被依赖**：无

## 注意事项

- 命令会持续打印捕获数据，默认 10 秒后自动结束并关闭设备，期间请勿重复执行同通道测试
- 统计信息（频率/占空比）由驱动在每次捕获事件时刷新，经设备 `user_data` 暴露给本测试
