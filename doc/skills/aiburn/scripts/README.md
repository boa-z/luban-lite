# AIBURN 烧录脚本

## 快速开始

**零安装！** 脚本自动定位 `aiburn/` 目录，无需 `pip install`。

### 基础用法（推荐）

```bash
# 烧录固件
python {SKILL_DIR}/scripts/aiburn.py --image firmware.img

# 读取串口日志（30秒）
python {SKILL_DIR}/scripts/aiburn.py readlog --duration 30

# 发送 shell 命令
python {SKILL_DIR}/scripts/aiburn.py sendcmd --command "free"

# 获取设备状态
python {SKILL_DIR}/scripts/aiburn.py getstatus
```

脚本自动读取项目根目录的 `aiburnconfig.json` 配置。

### 启用串口日志

```bash
python {SKILL_DIR}/scripts/aiburn.py \
    --image firmware.img \
    --monitor COM6
```

### 覆盖配置参数

```bash
python {SKILL_DIR}/scripts/aiburn.py \
    --image firmware.img \
    --host 172.16.31.178 \
    --port 9100 \
    --monitor COM6 \
    --baudrate 115200
```

**优先级：** CLI 参数 > 配置文件 > 默认值

---

## 配置文件

在项目根目录创建 `aiburnconfig.json`：

```json
{
    "server": {
        "host": "localhost",
        "port": 9100
    },
    "device": {
        "monitor_port": "COM5",
        "baudrate": 115200
    }
}
```

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `server.host` | upgcmd Server IP | `localhost` |
| `server.port` | upgcmd Server 端口 | `9100` |
| `device.monitor_port` | 串口日志端口 | 空=不启用 |
| `device.baudrate` | 波特率 | `115200` |

---

## 输出格式

结构化 JSON（stdout），调试信息输出到 stderr。

### 烧录模式

```json
{
  "success": true,
  "status": "success",
  "exit_code": 0,
  "duration_ms": 12345,
  "summary": "Burn completed successfully",
  "error": "",
  "burn_logs_count": 150,
  "boot_logs_count": 200,
  "burn_analysis": {
    "ddr_info": "DDR2 @ 504MHz, 64MB",
    "nftl_status": "OK",
    "summary": "烧录完成，6个分区已写入"
  },
  "boot_analysis": {
    "firmware_version": "1.0.0",
    "os_version": "ArtInChip Luban-Lite 1.3.1",
    "startup_time": "1.772s",
    "shell_prompt": true,
    "summary": "启动成功，固件 1.0.0，Shell 就绪"
  }
}
```

---

## 常见问题

### Q: 找不到 aiburnconfig.json？

A: 脚本会使用默认值（localhost:9100），但建议创建配置文件。

### Q: ImportError: No module named 'aiburn'？

A: 确保脚本位于 `{SKILL_DIR}/scripts/aiburn.py`，且 `aiburn/` 目录在 `{SKILL_DIR}/scripts/aiburn/`。脚本会自动定位，无需 pip install。

---

## 更多文档

- [SKILL.md](../SKILL.md) - Skill 描述文件
- [README.md](../README.md) - Skill 安装指南
