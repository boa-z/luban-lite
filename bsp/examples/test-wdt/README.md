# 看门狗测试 (test-wdt)

## 一句话描述

WDT（Watchdog Timer）看门狗定时器测试，包含集成测试和基础超时重启测试。

## 调用关系

```
MSH Shell → test-wdt → AIC WDT 驱动
```

## 目录结构

| 文件 | 职责 |
|------|------|
| `test_wdt.c` | 看门狗基础测试（超时重启、预超时中断、喂狗） |
| `SConscript` | 编译脚本 |

## 配置方法

| Kconfig | 说明 | 默认值 | 依赖 |
|---------|------|--------|------|
| `AIC_WDT_DRV_TEST` | 启用 WDT 驱动测试命令 | n | AIC_WDT_DRV && !DRIVER_BARE_DRV_EN |

通过 `scons --menuconfig` 开启：

```
menuconfig
  └─ Drivers options
      └─ Drivers examples
          └─ Enable WDT driver test command
```

## 使用说明

### `test_wdt` — 看门狗超时重启测试

**参数说明：**

| 参数 | 说明 |
|------|------|
| `-s, --set-timeout <sec>` | 设置超时时间（秒），并启动看门狗 |
| `-p, --set-pretimeout <sec>` | 设置预超时时间（秒），注册中断回调并开中断 |
| `-c, --set-clear <sec>` | 设置清除阈值（秒）—— 计数器低于此值时喂狗会被硬件忽略 |
| `-w, --write-protect <0~3>` | 设置寄存器写保护等级 |
| `-g, --get-timeout` | 读取当前超时时间，并作为命令返回值 |
| `-k, --keepalive` | 起一个线程反复喂狗 |
| `-h, --help` | 显示帮助 |

**使用示例：**

```bash
# 设置 10 秒超时（不喂狗，10 秒后系统复位）
test_wdt -s 10

# 超时 10 秒，预超时 5 秒
test_wdt -s 10 -p 5

# 超时 3 秒，起线程喂狗 5 次
test_wdt -k -s 3

# 读取当前超时时间
test_wdt -g

```

## 集成测试

```bash
itest_wdt
```

依次跑「设超时 / 读超时 / 设预超时 / 喂狗」几组用例并断言结果。

## 依赖说明

- **上游依赖**：AIC WDT 驱动
- **下游被依赖**：无
