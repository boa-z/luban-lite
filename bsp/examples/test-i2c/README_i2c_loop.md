# I2C 主从机回环测试 (i2c_loop)
## 一、简介
I2C 主从机回环测试工具，将 SoC 内部两个 I2C 控制器直接连接为主机(Master)+从机(Slave)，通过 SDA/SCL 短接实现自发自收，验证 I2C 控制器在多速率、stuck 阈值、包长、7bit/10bit地址模式、combined格式、长时间压力下的信号和功能可靠性。

测试覆盖项：
- 7bit/10bit 地址模式基础通信
- 100k/400k 两档标准速率遍历
- SDA/SCL stuck 阈值遍历（50~2000 cycles）
- 1~255 字节全包长遍历
- Write-then-Read combined格式（restart）
- 250次长时间压力稳定性测试
- 一键全量自动化测试
- 标准itest入口，支持CI系统自动调用

## 二、运行环境
- ArtInChip Luban-Lite SDK（RT-Thread）
- 硬件要求：I2C0 与 I2C1 的 SDA、SCL 对应短接，GND必须共地
- 驱动支持：使能 I2C 驱动 `AIC_I2C_DRV`

## 三、项目目录结构
| 文件 | 位置 |
|------|------|
| `test_i2c_loop.c` | I2C 主从回环测试主程序 |
| `itest_i2c.c` | itest自动化测试入口 |
| `SConscript` | 构建脚本 |
| `i2c_loop_README.md` | 使用说明文档 |

## 四、配置使用说明
### Kconfig 选项
| Kconfig | 说明 | 默认值 | 依赖 |
|---------|------|--------|------|
| `AIC_I2C_DRV` | 使能 I2C 驱动 | n | - |
| `AIC_I2C0` | 使能 I2C0 控制器（作为Slave） | y | AIC_I2C_DRV |
| `AIC_I2C1` | 使能 I2C1 控制器（作为Master） | y | AIC_I2C_DRV |
| `I2C DRIVER TEST COMMAND` | 使能I2C测试命令 | n | AIC_I2C_DRV, RT_USING_FINSH |

通过 `scons --menuconfig` 使能配置：
```
menuconfig
  → Board options
      → Enable I2C 
          → Enable I2C0 
          → Enable I2C1
  → Drivers options
      → Drivers examples
          → Enable I2C driver test command
```

### 地址模式说明
I2C0 与 I2C1 的7bit/10bit地址模式，在 menuconfig 中选择，程序在 `init` 时自动读取当前硬件配置，无需强制修改。
- 当硬件为7bit/10bit模式，`test` 命令应 PASS
- 主从模式不匹配时，`test`应 FAIL，真实反映硬件通信状态

### 硬件连接
| I2C1 (Master) | I2C0 (Slave) |
|---------------|--------------|
| SDA | SDA |
| SCL | SCL |
| GND | GND |

## 五、使用说明
所有命令在 MSH Shell 以 `i2c_loop <子命令>` 形式执行，使用前必须先执行 `init` 初始化环境。

### `init` - 初始化测试环境
初始化 I2C 设备，注册 Slave 回调函数，自动读取驱动中配置的速率、地址、stuck阈值，自动读取当前 menuconfig 配置的地址模式。
**使用示例：**
```bash
i2c_loop init
```
输出示例：
```
[OK] Init success: 7bit addr=0x0A speed=400000Hz stuck=400 cycles
```

### `deinit` - 释放资源
关闭 I2C 设备，释放占用的资源。
**使用示例：**
```bash
i2c_loop deinit
```

### `test` - 全量自动化测试
在**当前硬件配置**下，执行全量测试，包括基础通信、组合格式、速率遍历、stuck遍历、包长遍历、250次压力测试，不修改地址模式。
- 当硬件为7bit/10bit模式 PASS
- 模式不匹配 FAIL，真实反映信号行为
**使用示例：**
```bash
i2c_loop test
```

### `speed <hz>` - 指定速率测试
临时修改 Master 时钟速率，执行16字节包长回环测试，测试完成后恢复原速率。支持100000(100k)、400000(400k) 两档标准速率。
**参数说明：**
| 参数 | 说明 |
|------|------|
| `<hz>` | 目标速率，单位Hz，可选100000/400000 |
**使用示例：**
```bash
i2c_loop speed 100000
```

### `stuck <cycles>` - 指定 stuck 阈值测试
临时修改 Master 和 Slave 的 SDA/SCL stuck 检测阈值，执行16字节包长回环测试，测试完成后恢复原值。
**参数说明：**
| 参数 | 说明 |
|------|------|
| `<cycles>` | stuck检测阈值，单位波特周期，建议范围50~2000 |
**使用示例：**
```bash
i2c_loop stuck 200
```

### `len <n>` - 指定包长测试
在当前配置下执行指定包长的回环测试。
**参数说明：**
| 参数 | 说明 |
|------|------|
| `<n>` | 测试包长，单位字节，范围1~255 |
**使用示例：**
```bash
i2c_loop len 64
```

### `writeread` - Combined格式测试（write-then-read/restart）
测试 I2C combined 格式：先写1字节寄存器地址（0x00），发送 restart 不发STOP，再读16字节数据。验证寄存器访问类I2C设备常用的传输时序，测试完成后恢复原配置。
**使用示例：**
```bash
i2c_loop writeread
```

### `speedsweep` - 速率遍历测试
在当前配置下，依次遍历100kHz、400kHz两档标准速率，执行16字节包长测试，统计通过情况，测试完成后恢复原速率。
**使用示例：**
```bash
i2c_loop speedsweep
```
输出示例：
```
###### SPEED SWEEP (2 levels: 100k/400k) ######
[1/2] 100000 Hz: PASS
[2/2] 400000 Hz: PASS
###### SPEED RESULT: 2/2 ######
```

### `stucksweep` - Stuck阈值遍历测试
在当前配置下，依次遍历50/100/200/400/800/1000/2000 cycles七档stuck检测阈值，执行16字节包长测试，统计通过情况，测试完成后恢复原值。
**使用示例：**
```bash
i2c_loop stucksweep
```

### `lensweep` - 包长遍历测试
在当前配置下，依次遍历1/2/4/8/16/32/64/128/255字节各种包长，执行回环测试，统计通过情况。
**使用示例：**
```bash
i2c_loop lensweep
```

### `itest_i2c` - CI自动化测试入口
标准itest命令，供CI系统自动调用，自动执行init→全量test→deinit流程，输出标准itest报告。
**使用示例：**
```bash
itest_i2c
```

## 六、注意事项说明
- **地址模式匹配原则**：I2C 通信需要 Master 和 Slave 地址模式一致，同为7bit或同为10bit，模式不匹配时 Slave 不回ACK，测试应 FAIL，以确保测试结果为真实硬件行为，不掩盖bug
- **测试数据模式**：发送数据采用递增模式（0x00, 0x01, 0x02, ...），接收后逐字节比对，任意字节错误即判定失败
- **Slave回调注意事项**：ArtInChip I2C Slave 每读/写1字节触发一次中断，代码通过 `g_read_active` 标志位，仅在新传输第一个字节重置offset，避免每字节触发READ_REQUESTED时offset清零导致数据错位
- **热寄存器修改原则**：临时修改stuck等寄存器时，遵循 `disable → 修改 → enable` 顺序，禁止热写寄存器导致硬件死锁或状态异常
- **支持速率说明**：硬件支持100kHz/400kHz两档标准I2C速率，非标准速率可能导致采样异常或通信错误
