"""
skill_burn.py - Firmware Burn SKILL

Sends burn commands to upgcmd Server via TCP protocol.
Supports dynamic serial port and baudrate configuration.
Auto-detects agent environment (local vs remote) and uploads firmware if needed.

Usage:
    from aiburn.skill_burn import burn_firmware
    result = burn_firmware(
        image_path='/path/to/firmware.bin',
        serial_port='COM4',
        baudrate=115200,
        host='localhost',
        port=9100
    )
"""

import sys
import time
import os
import re
from typing import Optional, Dict, Any
from .aicupg_client import AICUPGClient
from .env_detector import detect_agent_environment

DEFAULT_HOST = 'localhost'
DEFAULT_PORT = 9100
DEFAULT_BAUDRATE = 115200
DEFAULT_TIMEOUT = 300


def burn_firmware(
    image_path: str,
    serial_port: str = '',
    baudrate: int = DEFAULT_BAUDRATE,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    timeout: int = DEFAULT_TIMEOUT,
    monitor_port: str = ''
) -> Dict[str, Any]:
    """
    Burn firmware to target device via upgcmd Server.

    Auto-detects agent environment:
    - Local agent (Windows): Uses local path directly
    - Remote agent (Linux/macOS): Uploads firmware to Windows first, then burns

    Serial Log Two-Phase Flow (when monitor_port is provided):
    1. Start serial monitor before burn
    2. After burn → collect_logs() → burn-phase logs
    3. send_cmd("reset") → device reboots
    4. After reboot → collect_logs() → boot-phase logs
    5. Monitor persists until explicitly stopped or Server exits (no auto-stop)

    Args:
        image_path:  Path to firmware image file (local or remote)
        serial_port: USB device port (e.g. '1:13') for burn/config
        baudrate:    Serial baudrate (default 115200)
        host:        upgcmd Server host (default 'localhost')
        port:        upgcmd Server port (default 9100)
        timeout:     Timeout in seconds (default 300)
        monitor_port: UART COM port for serial log monitoring (e.g. 'COM5').
                      If provided, enables two-phase serial log collection.

    Returns:
        Dict with keys: success, status, exit_code, duration_ms, summary, logs
    """
    result = {
        'success': False,
        'status': 'unknown',
        'exit_code': -1,
        'duration_ms': 0,
        'summary': '',
        'logs': [],
        'error': ''
    }

    if not image_path:
        result['error'] = 'image_path is required'
        return result

    print(f"[SKILL:BURN] Detecting agent environment...")
    env = detect_agent_environment(server_ip=host)
    print(f"[SKILL:BURN] Environment: {env['agent_os']} agent → {env['server_ip']} server")
    print(f"[SKILL:BURN] Is remote: {env['is_remote']}, needs_transfer: {env['needs_transfer']}")

    actual_image_path = image_path

    if env['is_remote']:
        print(f"[SKILL:BURN] ️  Remote agent detected!")
        print(f"[SKILL:BURN] Querying server for optimal upload directory...")

        client_upload = AICUPGClient(host, port)
        if not client_upload.connect():
            result['error'] = f'Failed to connect to upgcmd Server at {host}:{port}'
            return result

        try:
            remote_dir = client_upload.get_upload_dir()
            if not remote_dir:
                result['error'] = 'Failed to get upload directory from server'
                return result

            print(f"[SKILL:BURN] Server will store firmware at: {remote_dir}")

            upload_result = client_upload.upload_firmware(
                local_path=image_path,
                remote_dir=remote_dir,
                chunk_size=8192
            )

            if not upload_result or upload_result.get('status') != 'success':
                result['error'] = f'Firmware upload failed: {upload_result}'
                return result

            actual_image_path = upload_result.get('remote_path', '')
            print(f"[SKILL:BURN] ✅ Firmware uploaded to: {actual_image_path}")

        except Exception as e:
            result['error'] = f'Firmware upload error: {str(e)}'
            return result
        finally:
            client_upload.disconnect()
    else:
        print(f"[SKILL:BURN] ✅ Local agent detected, using local path directly")
        actual_image_path = image_path

    print(f"[SKILL:BURN] Image: {actual_image_path}")
    print(f"[SKILL:BURN] Server: {host}:{port} (local)")
    print(f"[SKILL:BURN] Device: {serial_port or 'auto-detect'}")

    client = AICUPGClient(host, port)
    if not client.connect():
        result['error'] = f'Failed to connect to upgcmd Server at {host}:{port}'
        return result

    try:
        actual_serial_port = serial_port
        if not serial_port or serial_port.strip() == '':
            print(f"[SKILL:BURN] No serial port specified, auto-detecting USB devices...")

            device_list = client.get_device_list()
            if not device_list or len(device_list) == 0:
                result['error'] = 'No USB device found. Please connect a device and retry.'
                return result

            actual_serial_port = device_list[0]['port']
            print(f"[SKILL:BURN] ✅ Auto-selected USB device: {actual_serial_port}")
        else:
            print(f"[SKILL:BURN] Using specified serial port: {serial_port}")

        max_config_retries = 3
        config_result = None

        for retry in range(max_config_retries):
            if retry > 0:
                wait_time = 2 * retry
                print(
                    f"[SKILL:BURN] Config failed, waiting {wait_time}s before retry "
                    f"({retry + 1}/{max_config_retries})..."
                )
                time.sleep(wait_time)

                if not serial_port or serial_port.strip() == '':
                    print(f"[SKILL:BURN] Re-detecting USB devices...")
                    device_list = client.get_device_list()
                    if not device_list or len(device_list) == 0:
                        result['error'] = (
                            'No USB device found after retry. '
                            'Device may have disconnected.'
                        )
                        return result
                    actual_serial_port = device_list[0]['port']
                    print(f"[SKILL:BURN] ✅ Re-selected USB device: {actual_serial_port}")

            config_result = client.config_device(actual_serial_port, baudrate)
            if config_result and config_result.get('status') == 'success':
                break

            error_code = config_result.get('error', {}).get('code', 0) if config_result else 0
            if error_code not in [-4, -5]:
                break

        if not config_result or config_result.get('status') != 'success':
            result['error'] = (
                f'Failed to configure device after {max_config_retries} retries: '
                f'{config_result}'
            )
            return result

        print(f"[SKILL:BURN] Device configured successfully")

        monitor_started = False
        if monitor_port and monitor_port.strip():
            print(f"[SKILL:BURN] Starting serial log monitor on {monitor_port} @ {baudrate}...")
            mon_result = client.start_log_monitor(monitor_port, baudrate)
            if mon_result and mon_result.get('status') == 'success':
                monitor_started = True
                print(f"[SKILL:BURN] \u2705 Serial log monitor started")
            else:
                print(f"[SKILL:BURN] Warning: Failed to start serial log monitor: {mon_result}")
                print(f"[SKILL:BURN] Continuing without serial log monitoring")

        print(f"[SKILL:BURN] Starting burn...")
        burn_result = client.burn_image(
            image_path=actual_image_path,
            partition_mode='all'
        )

        if not burn_result:
            result['error'] = 'Burn failed: no response from server'
            return result

        result['exit_code'] = 0 if burn_result.get('status') == 'success' else -1
        result['duration_ms'] = int(burn_result.get('duration_ms', 0))
        result['summary'] = burn_result.get('message', '')

        if burn_result.get('status') == 'success':
            result['success'] = True
            result['status'] = 'success'
            print(f"[SKILL:BURN] Burn completed successfully")

            if monitor_started:
                # ===== Serial log two-phase flow =====

                print(f"[SKILL:BURN] Phase 1: Collecting burn-phase serial logs (3s)...")
                time.sleep(2)
                burn_logs = client.collect_logs(duration_sec=3.0)
                print(f"[SKILL:BURN] Burn-phase logs: {len(burn_logs)} lines")

                print(f"[SKILL:BURN] Sending reset command to device...")
                reset_result = client.send_cmd("reset")
                if reset_result and reset_result.get('status') == 'success':
                    print(f"[SKILL:BURN] \u2705 Reset command sent successfully")
                else:
                    print(f"[SKILL:BURN] Warning: Reset command result: {reset_result}")

                print(f"[SKILL:BURN] Waiting for device reboot (12s)...")
                time.sleep(12)

                print(f"[SKILL:BURN] Phase 2: Collecting boot-phase serial logs (6s)...")
                boot_logs = client.collect_logs(duration_sec=6.0, stop_on_pattern="# ")
                print(f"[SKILL:BURN] Boot-phase logs: {len(boot_logs)} lines")

                all_logs = burn_logs + boot_logs
                result['logs'] = all_logs
                result['burn_logs'] = burn_logs
                result['boot_logs'] = boot_logs
                result['log_source'] = 'serial_monitor'
                print(f"[SKILL:BURN] Total serial logs: {len(all_logs)} lines "
                      f"(burn={len(burn_logs)}, boot={len(boot_logs)})")
            else:
                print(f"[SKILL:BURN] Waiting for burn completion (2s)...")
                time.sleep(2)

                print(f"[SKILL:BURN] Fetching boot logs via USB get_log...", flush=True)
                try:
                    usb_logs = _fetch_usb_log(client, actual_serial_port, baudrate)
                    if usb_logs:
                        result['logs'] = usb_logs
                        result['log_source'] = 'usb_get_log'
                        print(f"[SKILL:BURN] USB logs captured: {len(usb_logs)} lines", flush=True)
                    else:
                        print(f"[SKILL:BURN] Warning: No USB logs available", flush=True)
                        result['logs'] = []
                except Exception as e:
                    print(f"[SKILL:BURN] Warning: Failed to fetch USB logs: {e}", flush=True)
                    result['logs'] = []

                print(
                    f"[SKILL:BURN] Please manually reset the device: upgcmd shcmd reset",
                    flush=True
                )
        else:
            result['status'] = 'failed'
            result['error'] = burn_result.get('error', {}).get('message', 'Burn failed')

    except Exception as e:
        result['error'] = f'Unexpected error: {str(e)}'
    finally:
        print(f"[SKILL:BURN] Disconnecting from server...")
        client.disconnect()
        print(f"[SKILL:BURN] Disconnected")

    if result.get('success'):
        burn_l = result.get('burn_logs', [])
        boot_l = result.get('boot_logs', [])

        if result.get('log_source') == 'serial_monitor':
            if burn_l:
                result['burn_analysis'] = _analyze_burn_logs(burn_l)
                print(f"[SKILL:BURN] Burn-phase analysis completed")
            if boot_l:
                result['boot_analysis'] = _analyze_boot_logs(boot_l)
                print(f"[SKILL:BURN] Boot-phase analysis completed")
        elif result.get('log_source') == 'usb_get_log':
            if burn_l:
                result['burn_analysis'] = _analyze_burn_logs(burn_l)
                print(f"[SKILL:BURN] USB burn-phase analysis completed")
                result['note'] = (
                    'USB mode: No boot logs available. '
                    'Please manually reset and check logs.'
                )
        else:
            if not result.get('logs'):
                result['note'] = 'No logs available for analysis'
                print(f"[SKILL:BURN] Note: No logs for analysis")

    print(f"[SKILL:BURN] Result: {result['status']} "
          f"(exit={result['exit_code']}, {result['duration_ms']}ms)")
    return result


def _fetch_usb_log(client, serial_port: str, baudrate: int, max_retries=3) -> list:
    """
    Retrieve boot logs via USB get_log interface.

    Args:
        client: AICUPGClient instance
        serial_port: Device port (for re-configuring device)
        baudrate: Baud rate
        max_retries: Max retry count (default 3)

    Returns:
        List of log lines
    """
    for retry in range(max_retries):
        try:
            if serial_port:
                print(f"[SKILL:BURN] Re-configuring device for USB log retrieval "
                      f"(attempt {retry + 1}/{max_retries})...")
                config_result = client.config_device(serial_port, baudrate)

                if not config_result or config_result.get('status') != 'success':
                    if retry < max_retries - 1:
                        wait_time = 2 * (retry + 1)  # Incremental wait: 2s, 4s, 6s
                        print(f"[SKILL:BURN] Config failed, waiting {wait_time}s before retry...")
                        time.sleep(wait_time)
                        continue
                    else:
                        print(
                            f"[SKILL:BURN] Warning: Failed to re-config device after "
                            f"{max_retries} attempts"
                        )
                        return []

                time.sleep(0.5)

            log_result = client.get_log(timeout=10)

            if log_result and log_result.get('status') == 'success':
                log_data = log_result.get('data', {})
                if 'log_lines' in log_data:
                    lines = log_data['log_lines']
                    print(f"[SKILL:BURN] USB log: {len(lines)} lines")
                    return lines
                elif 'log' in log_data:
                    log_str = log_data['log']
                    if log_str:
                        lines = log_str.splitlines()
                        print(f"[SKILL:BURN] USB log: {len(lines)} lines")
                        return lines
                return []
            else:
                if retry < max_retries - 1:
                    print(f"[SKILL:BURN] get_log failed, retrying in {2 * (retry + 1)}s...")
                    time.sleep(2 * (retry + 1))
                else:
                    print(f"[SKILL:BURN] USB log not available after {max_retries} retries")
                    return []

        except Exception as e:
            if retry < max_retries - 1:
                print(f"[SKILL:BURN] USB log fetch error (retry {retry + 1}): {e}")
                time.sleep(2)
            else:
                print(f"[SKILL:BURN] USB log fetch failed after {max_retries} retries: {e}")
                return []

    return []


def _strip_ansi(text):
    """Strip ANSI escape sequences (e.g. \x1b[32m)."""
    return re.sub(r'\x1b\[[0-9;]*m', '', text)


def _analyze_burn_logs(logs):
    """
    Burn-phase log analysis.

    Args:
        logs: List of log lines from burn phase

    Returns:
        Dict with burn-phase analysis results
    """
    analysis = {
        'ddr_info': None,
        'spl_version': None,
        'reset_reason': None,
        'qspi_freq': None,
        'components': [],
        'nftl_status': None,
        'upgrade_complete': False,
        'errors': [],
        'warnings': [],
        'summary': ''
    }

    for raw_line in logs:
        line = _strip_ansi(raw_line).strip()
        if not line:
            continue
        line_lower = line.lower()

        if 'going to init ddr' in line_lower:
            freq_match = re.search(r'freq:\s*(\d+)\s*mhz', line_lower)
            ddr_type = 'DDR2' if 'ddr2' in line_lower else 'DDR3' if 'ddr3' in line_lower else 'DDR'
            freq = freq_match.group(1) if freq_match else 'unknown'
            analysis['ddr_info'] = f'{ddr_type} @ {freq} MHz'
        elif 'dram size' in line_lower:
            size_match = re.search(r'(\d+)\s*mb', line_lower)
            if size_match and analysis['ddr_info']:
                analysis['ddr_info'] += f', {size_match.group(1)} MB'

        if 'pre-boot program' in line_lower:
            ver_match = re.search(r'\((.+?)\)', line)
            if ver_match:
                analysis.setdefault('pbp_version', ver_match.group(1))
        if 'tinyspl' in line_lower:
            built_match = re.search(r'built on (.+?)\]', line, re.IGNORECASE)
            if built_match:
                analysis['spl_version'] = built_match.group(1).strip()

        if 'reset action' in line_lower:
            analysis['reset_reason'] = line.split(':', 1)[-1].strip() if ':' in line else line

        if 'qspi' in line_lower and 'freq' in line_lower:
            freq_match = re.search(r'(\d+)\s*hz', line_lower)
            if freq_match:
                analysis['qspi_freq'] = f"{int(freq_match.group(1)):,} Hz"

        if 'name:' in line_lower and 'image.' in line_lower:
            comp_name = line.split(':', 1)[-1].strip()
            analysis['components'].append(comp_name)

        if 'nftl' in line_lower:
            if 'nftl ok' in line_lower or 'nftl lib version' in line_lower:
                analysis['nftl_status'] = 'OK'
            elif 'failed' in line_lower or 'error' in line_lower:
                analysis['nftl_status'] = 'FAILED'

        if 'end of upgrading' in line_lower:
            analysis['upgrade_complete'] = True

        if re.search(r'\[e/\w+\]', line_lower) or 'error' in line_lower or 'failed' in line_lower:
            harmless = ['read id failed', 'read info failed']
            is_harmless = any(h in line_lower for h in harmless)
            if not is_harmless:
                analysis['errors'].append(line)
            else:
                analysis['warnings'].append(line)

    parts = []
    if analysis['ddr_info']:
        parts.append(f"DDR: {analysis['ddr_info']}")
    if analysis['components']:
        parts.append(f"{len(analysis['components'])} partitions written")
    if analysis['nftl_status']:
        parts.append(f"NFTL {analysis['nftl_status']}")

    if analysis['upgrade_complete'] and not analysis['errors']:
        analysis['summary'] = f"✅ Burn completed, {', '.join(parts)}"
    elif analysis['errors']:
        analysis['summary'] = f"⚠️ Burn error ({len(analysis['errors'])} errors)"
    else:
        analysis['summary'] = (
            f"✅ Burn completed ({', '.join(parts)})"
            if parts
            else "✅ Burn completed"
        )

    return analysis


def _analyze_boot_logs(logs):
    """
    Boot-phase log analysis.

    Args:
        logs: List of log lines from boot phase

    Returns:
        Dict with boot-phase analysis results
    """
    analysis = {
        'ddr_info': None,
        'spl_load': None,
        'crc_check': None,
        'board_info': None,
        'firmware_version': None,
        'os_version': None,
        'build_time': None,
        'startup_time': None,
        'shell_prompt': False,
        'boot_device': None,
        'reset_reason': None,
        'nftl_status': None,
        'peripherals': [],
        'errors': [],
        'warnings': [],
        'summary': ''
    }

    for raw_line in logs:
        line = _strip_ansi(raw_line).strip()
        if not line:
            continue
        line_lower = line.lower()

        if 'going to init ddr' in line_lower:
            freq_match = re.search(r'freq:\s*(\d+)\s*mhz', line_lower)
            ddr_type = 'DDR2' if 'ddr2' in line_lower else 'DDR3' if 'ddr3' in line_lower else 'DDR'
            freq = freq_match.group(1) if freq_match else 'unknown'
            analysis['ddr_info'] = f'{ddr_type} @ {freq} MHz'
        elif 'dram size' in line_lower:
            size_match = re.search(r'(\d+)\s*mb', line_lower)
            if size_match and analysis['ddr_info']:
                analysis['ddr_info'] += f', {size_match.group(1)} MB'

        if 'spl read' in line_lower:
            speed_match = re.search(r'(\d+)\s*kb/s', line_lower)
            bytes_match = re.search(r'(\d+)\s*byte', line_lower)
            if speed_match:
                analysis['spl_load'] = f'{speed_match.group(1)} KB/s'
                if bytes_match:
                    b = int(bytes_match.group(1))
                    analysis['spl_load'] += f' ({b:,} bytes)'

        if 'crc32' in line_lower:
            if 'ok' in line_lower or 'pass' in line_lower or 'verify ok' in line_lower:
                analysis['crc_check'] = 'PASSED'
            else:
                analysis['crc_check'] = 'FAILED'

        if 'board:' in line_lower:
            analysis['board_info'] = line.split(':', 1)[-1].strip()

        if 'welcome to artinchip' in line_lower or 'luban' in line_lower:
            ver_match = re.search(r'(\d+\.\d+\.\d+)', line)
            if ver_match:
                analysis['os_version'] = f'ArtInChip Luban-Lite {ver_match.group(1)}'
        if 'image version' in line_lower:
            analysis['firmware_version'] = line.split(':', 1)[-1].strip()
        if 'built on' in line_lower and ('welcome' in line_lower or 'artinchip' in line_lower):
            built_match = re.search(r'built on (.+)', line)
            if built_match:
                analysis['build_time'] = built_match.group(1).strip()

        if 'startup time' in line_lower:
            time_match = re.search(r'([\d.]+)\s*sec', line_lower)
            if time_match:
                analysis['startup_time'] = f'{time_match.group(1)}s'

        if line.strip() in ['aic />', 'aic>', '# ', '$ '] or line.strip().endswith('/>'):
            analysis['shell_prompt'] = True

        if 'boot device' in line_lower:
            analysis['boot_device'] = line.split('=', 1)[-1].strip() if '=' in line else line

        if 'reset action' in line_lower:
            analysis['reset_reason'] = line.split(':', 1)[-1].strip() if ':' in line else line

        if 'nftl' in line_lower:
            if 'nftl ok' in line_lower:
                analysis['nftl_status'] = 'OK'
            elif 'nftl_recover' in line_lower:
                analysis['nftl_status'] = 'OK (recovered)'
            elif 'failed' in line_lower or 'error' in line_lower:
                analysis['nftl_status'] = 'FAILED'

        if re.search(r'\[i/\w+\]', line_lower):
            periph_match = re.search(r'\[i/(\w+)\]', line_lower)
            if periph_match:
                periph = periph_match.group(1)
                desc = line.split(']')[-1].strip() if ']' in line else line
                analysis['peripherals'].append(f'{periph}: {desc[:60]}')

        if re.search(r'\[e/\w+\]', line_lower) or 'error' in line_lower or 'failed' in line_lower:
            harmless = ['read id failed', 'read info failed', 'mount fs[elm]']
            is_harmless = any(h in line_lower for h in harmless)
            if not is_harmless:
                analysis['errors'].append(line)
            else:
                analysis['warnings'].append(line)
        elif 'warning' in line_lower or 'warn' in line_lower:
            analysis['warnings'].append(line)

    parts = []
    if analysis['board_info']:
        parts.append(f"Board: {analysis['board_info']}")
    if analysis['firmware_version']:
        parts.append(f"Firmware: {analysis['firmware_version']}")
    if analysis['startup_time']:
        parts.append(f"Time: {analysis['startup_time']}")
    if analysis['shell_prompt']:
        parts.append("Shell ready")

    if analysis['shell_prompt'] and not analysis['errors']:
        analysis['summary'] = (
            f"✅ Boot successful ({', '.join(parts)})"
            if parts
            else "✅ Boot successful"
        )
    elif analysis['shell_prompt']:
        analysis['summary'] = f"✅ Boot successful ({len(analysis['errors'])} non-fatal errors)"
    elif analysis['errors']:
        analysis['summary'] = f"❌ Boot error ({len(analysis['errors'])} errors)"
    else:
        analysis['summary'] = "️ Boot logs captured but no shell prompt detected"

    return analysis
