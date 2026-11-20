---
name: aiburn
description: >
  烧录 ArtInChip 芯片固件到开发板的完整解决方案。

  **唯一入口：** `python {SKILL_DIR}/scripts/aiburn.py burn --image firmware.img`

  **设备管理子命令：**
  - `readlog [--duration N]` - 读取串口日志（默认30秒）
  - `sendcmd --command "CMD" [--serial]` - 发送 shell 命令（可选串口）
  - `getstatus` - 获取 USB 设备状态
license: Apache-2.0
requirements:
  - Python 3.7+
  - upgcmd Server (localhost:9100)
metadata:
  author: xingyang.liu <xingyang.liu@artinchip.com>
  version: "1.0.0"
---

# AIBURN Firmware Burn Skill

烧录 ArtInChip 芯片固件到开发板，自动重启并通过串口获取启动日志。

## AGENT 必读规则（违反会导致失败）

### 规则 0：必须且只能使用独立脚本

所有烧录、日志、命令操作**必须通过 `scripts/aiburn.py` 脚本完成**。禁止直接调用 SDK 或手写 TCP/socket 代码。

**例外：** 用户明确要求直接调用某个函数或模块来调试/排查问题时，可以按用户指示操作。

脚本失败时：检查命令参数和固件路径 → 查看 stderr 错误信息 → 不要尝试其他实现方式。

### 规则 1：严禁主动关闭 Monitor

烧录完成或发送串口命令后，**绝对不能**调用 `stop_log_monitor()`。Monitor 负责持续捕获串口输出，TCP 断开不影响 Monitor 运行，停止 Monitor 后将无法收到任何串口数据。

### 规则 2：执行前必须读取 aiburnconfig.json

每次执行脚本前，必须先读取工作目录下的 `aiburnconfig.json`，用其参数构造 CLI 命令：

```json
{
    "server": { "host": "172.16.31.178", "port": 9100 },
    "device": { "monitor_port": "COM5", "baudrate": 115200 }
}
```

| JSON 路径 | CLI 参数 | 说明 |
|-----------|---------|------|
| `server.host` | `--host` | Server IP |
| `server.port` | `--port` | Server 端口 |
| `device.monitor_port` | `--monitor` | 串口端口（如 COM5） |
| `device.baudrate` | `--baudrate` | 波特率（默认 115200） |

> `image_path` 由用户提供，`serial_port` 由自动检测完成。配置缺失或为空时**必须询问用户确认**，禁止猜测默认值。

## CLI 参数

| 参数 | 类型 | 说明 | 默认值 | 适用命令 |
|------|------|------|--------|----------|
| `--image` | str | 固件镜像路径，**必需** | 无 | burn |
| `--monitor` | str | 串口号（如 `COM5`），启用串口日志 | 空=不启用 | burn, readlog, sendcmd |
| `--host` | str | Server 地址 | `localhost` | 所有命令 |
| `--port` | int | Server 端口 | `9100` | 所有命令 |
| `--baudrate` | int | 波特率 | `115200` | burn, readlog, sendcmd |
| `--timeout` | int | 超时（秒） | `300` | 所有命令 |
| `--duration` | int | 日志收集时长（秒） | `30` | readlog |
| `--command` | str | Shell 命令，必需 | 无 | sendcmd |
| `--serial` | flag | 通过串口发送（自动启动 Monitor） | False | sendcmd |

> `readlog` 和 `sendcmd --serial` 会自动启动 Monitor（如未运行），无需先执行 `burn`。

## 子命令用法

**烧录固件**：

```bash
python {SKILL_DIR}/scripts/aiburn.py burn \
    --image output/d21x_demo88_nand.img \
    --host 172.16.31.178 --port 9100 \
    --monitor COM5 --baudrate 115200
```

**读取串口日志**（默认30秒）：

```bash
python {SKILL_DIR}/scripts/aiburn.py readlog --monitor COM5 --duration 60
```

**发送 shell 命令**（通过 USB / 通过串口）：

```bash
python {SKILL_DIR}/scripts/aiburn.py sendcmd --command "reset"
python {SKILL_DIR}/scripts/aiburn.py sendcmd --command "reboot" --serial --monitor COM5
```

**获取设备状态**：

```bash
python {SKILL_DIR}/scripts/aiburn.py getstatus
```

## 分析结果参考

**核心原则：** Agent 不需要解析原始日志数组，只需读取 `summary` 和关键字段，用自然语言组织回复。

### burn_analysis 字段

| 字段 | 类型 | 说明 | 示例值 |
|------|------|------|--------|
| `summary` | str | **烧录阶段总结**（最重要） | `"烧录完成，6个分区已写入，NFTL OK"` |
| `ddr_info` | str | DDR 信息 | `"DDR2 @ 504MHz, 64MB"` |
| `spl_version` | str | SPL 编译时间 | `"Jul  6 2026 17:11:52"` |
| `reset_reason` | str | Reset 原因 | `"Warm-Reset, reason: External-PIN-Reset"` |
| `qspi_freq` | str | QSPI 频率 | `"91,636,363 Hz"` |
| `components` | list | 已烧录分区列表 | `["spl", "env", "os", ...]` |
| `nftl_status` | str | NFTL 状态 | `"OK"` / `"FAILED"` |
| `upgrade_complete` | bool | 升级是否完成 | `True` / `False` |
| `errors` | list | 错误列表 | `[]` |
| `warnings` | list | 警告列表 | `["gt911 read id failed"]` |

### boot_analysis 字段

| 字段 | 类型 | 说明 | 示例值 |
|------|------|------|--------|
| `summary` | str | **启动阶段总结**（最重要） | `"启动成功，固件 1.0.0，耗时 1.77s，Shell 就绪"` |
| `firmware_version` | str | **固件版本**（用户最关心） | `"1.0.0"` |
| `startup_time` | str | **启动耗时**（用户最关心） | `"1.772s"` |
| `shell_prompt` | bool | **Shell 是否就绪**（用户最关心） | `True` / `False` |
| `os_version` | str | OS 名称和版本 | `"ArtInChip Luban-Lite 1.3.1"` |
| `board_info` | str | 板卡型号 | `"demo88-nand"` |
| `build_time` | str | 构建时间 | `"Jul  6 2026 17:12:06"` |
| `boot_device` | str | 启动设备 | `"BD_SPINAND"` |
| `ddr_info` | str | DDR 信息 | `"DDR2 @ 504MHz, 64MB"` |
| `spl_load` | str | SPL 加载速度 | `"7210 KB/s (1,279,928 bytes)"` |
| `crc_check` | str | CRC 校验结果 | `"PASSED"` / `"FAILED"` |
| `reset_reason` | str | Reset 原因 | `"Watchdog-Reset, reason: Command-Reboot"` |
| `nftl_status` | str | NFTL 状态 | `"OK (recovered)"` |
| `peripherals` | list | 外设列表 | `["USB EHCI/OHCI", "PSADC", ...]` |
| `errors` | list | 错误列表 | `[]` |
| `warnings` | list | 警告列表 | `["gt911 read id failed"]` |

### 回复要点

从 `summary` 和关键字段组织自然语言回复，按以下顺序报告：

- **烧录阶段**：DDR → SPL → 分区列表 → NFTL → 升级状态
- **启动阶段**：板卡 → OS → 固件版本 → 启动耗时 → 外设 → Shell 状态
- **错误**用 ❌ 标记，**警告**用 ⚠️ 标记（可忽略的已知警告：gt911 read id failed、sdcard mount failed）
- **关键指标**始终突出：固件版本、启动耗时、Shell 就绪状态

### 状态决策树

```
result['success'] == True?
├─ NO → 返回 result['error']，建议检查 USB 连接 / BROM 模式 / Server 状态
└─ YES
    ├─ burn_analysis?
    │   ├─ NO → USB 模式，无串口日志
    │   └─ summary 含 "✅" → 烧录成功 / 含 "❌" → 烧录失败
    └─ boot_analysis?
        ├─ NO → 未启用串口 Monitor
        └─ summary 含 "Shell 就绪" → 完全可用
           / 含 "启动成功" → 启动完成
           / 含 "❌" → 启动失败
```

## 完整流程（传 `monitor_port` 时）

```
1. 环境检测 → 远程 Agent 自动上传固件到 Windows Server
2. 启动串口 Monitor（在烧录前）
3. Config USB 设备
4. Burn Image（烧录固件）
5. Collect Logs（烧录期日志，3s）
6. Send "reset"（自动重启设备）
7. Wait 18s（等待设备重启 + USB 重新枚举）
8. Collect Logs（启动期日志，10s，遇到 "# " 提前停止）
9. 返回结果（burn_logs、boot_logs、burn_analysis、boot_analysis）
10. Disconnect TCP（Monitor 保持运行）
```

## 常见错误速查

| 错误信息 | 原因 | 解决方案 |
|---------|------|--------|
| `Failed to connect to localhost:9100` | Server 未运行 | 启动 `upgcmd.exe --server 9100` |
| `Firmware file not found` | 固件路径错误 | 检查路径是否正确 |
| `No device connected` | USB 未连接或未 BROM 模式 | 按住 BOOT 键上电，重新插拔 USB |
| `Burn failed: ERR_BURN_FAILED` | 烧录过程出错 | 检查串口终端输出 |
| `Timeout waiting for response` | 烧录超时 | 增加 timeout 参数或检查网络 |

## 返回结果格式（顶层结构）

```json
{
    "success": true,
    "status": "success",
    "exit_code": 0,
    "duration_ms": 12345,
    "summary": "...",
    "error": "",
    "burn_logs_count": 50,
    "boot_logs_count": 80,
    "burn_analysis": { "见上方 burn_analysis 字段表" },
    "boot_analysis": { "见上方 boot_analysis 字段表" }
}
```
