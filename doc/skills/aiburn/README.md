# AIBURN Skill

ArtInChip 芯片固件烧录解决方案，支持本地和远程 Agent 环境。

## 快速开始

**零安装！** Agent 只需执行一行命令：

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

脚本自动定位 `aiburn/` 目录，自动读取 `aiburnconfig.json`，无需 `pip install`。

---

## 目录结构

```
{SKILL_DIR}/
├── SKILL.md              ← Skill 描述（Agent 读取）
├── README.md             ← 当前文件
└── scripts/
    ├── aiburn.py         ← 烧录入口脚本
    ├── README.md         ← 脚本使用说明
    └── aiburn/           ← SDK 包目录
        ├── __init__.py
        ├── aicupg_client.py   ← TCP 客户端（帧协议）
        ├── env_detector.py    ← 环境检测（本地/远程）
        ├── skill_burn.py          ← 烧录核心逻辑
```

项目根目录还需要：

```
项目根目录/
├── aiburnconfig.json     ← 烧录配置（host/port/monitor_port）
└── upgcmd/
    └── upgcmd.exe        ← TCP Server
```

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

**优先级：** CLI 参数 > 配置文件 > 默认值

---

## 烧录流程

```
1. 环境检测 → 远程 Agent 自动上传固件到 Windows Server
2. 启动串口 Monitor（在烧录前）
3. Config USB 设备 → Burn Image → Collect Logs（3s）
4. Send "reset" → Wait 18s（重启 + USB 重新枚举）
5. Collect Logs（启动期，10s，遇到 "# " 提前停止）
6. 返回结果（burn_analysis + boot_analysis）
7. Disconnect TCP（Monitor 保持运行）
```

---

## 常见问题

| 问题 | 解决方案 |
|------|---------|
| 找不到 aiburnconfig.json | 脚本使用默认值 localhost:9100，建议创建配置文件 |
| `ImportError: No module named 'aiburn'` | 确保 `aiburn/` 在 `{SKILL_DIR}/scripts/aiburn/`，无需 pip install |
| COM 口被占用 | 关闭其他串口终端（PuTTY、MobaXterm） |
| 远程 Agent 无法烧录 | SDK 自动上传固件到 Server，无需手动处理 |
| upgcmd Server 未运行 | 执行 `upgcmd.exe --server 9100` |

---

## 相关文档

- [SKILL.md](SKILL.md) - Skill 描述文件
- [scripts/README.md](scripts/README.md) - 脚本使用说明
