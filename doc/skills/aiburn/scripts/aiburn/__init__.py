"""
aiburn - AI-powered firmware burn SDK for upgcmd

This package provides modules for burning firmware to ArtInChip devices:
- Burn firmware (skill_burn)

Architecture:
    Agent (Qoder/Claude IDE) -> TCP localhost:9100 -> upgcmd Server -> USB/UART -> Board

Quick start:
    from aiburn import burn_firmware

    # Burn firmware
    result = burn_firmware('/path/to/image.bin', serial_port='1:13')
"""

__version__ = '1.0.0'
__author__ = 'xingyang.liu <xingyang.liu@artinchip.com>'
__copyright__ = 'Copyright (c) 2020-2026 ArtInChip Technology Co., Ltd.'
__license__ = 'Apache-2.0'

from .skill_burn import burn_firmware
from .aicupg_client import AICUPGClient, test_connection
from .env_detector import detect_agent_environment, detect_os

__all__ = [
    'burn_firmware',
    'AICUPGClient',
    'test_connection',
    'detect_agent_environment',
    'detect_os',
]
