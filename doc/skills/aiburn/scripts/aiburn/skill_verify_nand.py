"""
skill_verify_nand.py - NAND One-Click Verification Workflow

Automated flow: modify defconfig -> compile firmware -> burn
-> send nand_verify -> analyze 3-phase logs

SDK path defaults to current working directory (user has already run 'lunch' to select board).
Board name is auto-read from .config, no extra configuration needed.

Usage:
    python {SKILL_DIR}/scripts/aiburn.py --verify-nand
"""

import os
import re
import glob
import time
import subprocess
from typing import Dict, Any, Tuple, List, Optional

from .skill_burn import burn_firmware, _strip_ansi, _analyze_burn_logs, _analyze_boot_logs


# ==================== Internal Utility Functions ====================

def _get_board_from_config(sdk_path: str = '.') -> str:
    """
    Read current board name from .config file.

    Parses CONFIG_PRJ_DEFCONFIG_FILENAME="xxx_defconfig" line,
    extracts board name (strips _defconfig suffix).

    Args:
        sdk_path: SDK root directory (default: current working directory)

    Returns:
        Board name string, e.g. 'd21x_demo88-nand_rt-thread_helloworld'

    Raises:
        FileNotFoundError: .config not found (user hasn't run 'lunch')
        ValueError: Board config not found in .config
    """
    config_file = os.path.join(sdk_path, '.config')

    if not os.path.exists(config_file):
        raise FileNotFoundError(
            f".config file not found at: {os.path.abspath(config_file)}\n"
            "Please run 'lunch' first to select a board type."
        )

    with open(config_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('CONFIG_PRJ_DEFCONFIG_FILENAME='):
                match = re.search(r'"([^"]+)"', line)
                if match:
                    board_config = match.group(1)
                    if board_config.endswith('_defconfig'):
                        return board_config[:-len('_defconfig')]
                    return board_config

    raise ValueError(
        "CONFIG_PRJ_DEFCONFIG_FILENAME not found in .config\n"
        "Please run 'lunch' first to select a board type."
    )


def enable_nand_verify_in_defconfig(sdk_path: str, board_name: str) -> Tuple[bool, str]:
    """
    Enable NAND verify command in defconfig.

    Adds CONFIG_AIC_NAND_VERIFY=y to defconfig file if not already present.

    Args:
        sdk_path: SDK root directory
        board_name: Board name

    Returns:
        (success, message)
    """
    defconfig_path = os.path.join(sdk_path, 'target', 'configs', f'{board_name}_defconfig')

    if not os.path.exists(defconfig_path):
        return False, f"defconfig not found: {defconfig_path}"

    with open(defconfig_path, 'r', encoding='utf-8') as f:
        content = f.read()

    if 'CONFIG_AIC_NAND_VERIFY=y' in content:
        return True, "CONFIG_AIC_NAND_VERIFY=y already enabled"

    if '# CONFIG_AIC_NAND_VERIFY is not set' in content:
        content = content.replace(
            '# CONFIG_AIC_NAND_VERIFY is not set',
            'CONFIG_AIC_NAND_VERIFY=y'
        )
    else:
        content = content.rstrip('\n') + '\nCONFIG_AIC_NAND_VERIFY=y\n'

    with open(defconfig_path, 'w', encoding='utf-8') as f:
        f.write(content)

    return True, "CONFIG_AIC_NAND_VERIFY=y added to defconfig"


def compile_firmware(sdk_path: str = '.', timeout: int = 120) -> Tuple[bool, str]:
    """
    Compile firmware using scons.

    Args:
        sdk_path: SDK root directory (default: current working directory)
        timeout: Timeout in seconds (default 120s)

    Returns:
        (success, message)
    """
    try:
        print(f"[SKILL:VERIFY] Compiling firmware in: {os.path.abspath(sdk_path)}")
        if os.name == 'nt':
            env = os.environ.copy()

            env['PYTHONIOENCODING'] = 'UTF-8'
            env['SDK_PRJ_TOP_DIR'] = sdk_path
            env['ENV_ROOT'] = os.path.join(sdk_path, 'tools', 'env')
            env['PKGS_ROOT'] = os.path.join(env['ENV_ROOT'], 'packages')
            env['RTT_ROOT'] = os.path.join(sdk_path, 'kernel', 'rt-thread')

            paths_to_add = [
                os.path.join(sdk_path, 'tools', 'env', 'tools', 'bin'),
                os.path.join(sdk_path, 'tools', 'env', 'tools', 'Python39'),
                os.path.join(sdk_path, 'tools', 'env', 'tools', 'Python27', 'Scripts'),
                os.path.join(sdk_path, 'tools', 'scripts', 'onestep'),
                os.path.join(sdk_path, 'toolchain', 'bin'),
            ]

            existing_paths = [p for p in paths_to_add if os.path.exists(p)]
            if existing_paths:
                current_path = env.get('PATH', '')
                env['PATH'] = os.pathsep.join(existing_paths) + os.pathsep + current_path
                print(f"[SKILL:VERIFY] Added {len(existing_paths)} SDK tool paths to PATH")

            cpu_count = os.cpu_count() or 4
            cmd = f'scons -C . -j{cpu_count}'
            use_shell = True
        else:
            cmd = ['scons', '-C', '.']
            use_shell = False
            env = None

        result = subprocess.run(
            cmd,
            cwd=sdk_path,
            capture_output=True,
            text=True,
            timeout=timeout,
            shell=use_shell,
            env=env
        )

        if result.returncode == 0:
            print(f"[SKILL:VERIFY] Compilation successful")
            return True, "Compilation successful"
        else:
            error_msg = result.stderr.strip() if result.stderr else result.stdout.strip()
            print(f"[SKILL:VERIFY] Compilation failed: {error_msg[:500]}")
            return False, f"Compilation failed:\n{error_msg[:500]}"

    except subprocess.TimeoutExpired:
        return False, f"Compilation timeout (> {timeout}s)"
    except FileNotFoundError:
        return False, "scons command not found. Please ensure scons is installed and in PATH."
    except Exception as e:
        return False, f"Compilation error: {str(e)}"


def _find_latest_image(sdk_path: str, board_name: str) -> Optional[str]:
    """
    Locate firmware from build output path.

    Path rule (from SConstruct): output/{board_name}/images/*.img
    Since compilation just finished, the path is known; no search needed.

    Args:
        sdk_path: SDK root directory
        board_name: Board name (from .config CONFIG_PRJ_DEFCONFIG_FILENAME, stripped of _defconfig)

    Returns:
        Firmware path, or None
    """
    images_dir = os.path.join(sdk_path, 'output', board_name, 'images')

    if not os.path.exists(images_dir):
        print(f"[SKILL:VERIFY] Output directory not found: {images_dir}")
        return None

    images = glob.glob(os.path.join(images_dir, '*.img'))
    if not images:
        print(f"[SKILL:VERIFY] No .img file in {images_dir}")
        return None

    latest_image = max(images, key=os.path.getmtime)
    print(f"[SKILL:VERIFY] Found firmware: {latest_image}")
    return latest_image


# ==================== NAND Verify Log Analysis ====================

def _analyze_nand_verify_logs(logs: List[str]) -> Dict[str, Any]:
    """
    Analyze NAND verification logs.

    Log format example:
        ========== NAND Phase Verify ==========
        [phase scan] delay mode: [no delay] test Failed!
        [phase scan] delay mode: [delay half cycle] test Success!
        [phase scan] delay mode: [delay one cycle] test Success!

        ========== NAND base Verify ==========
        single page read: 2048 byte, 281 us -> 7117 KB/s
        ...

        ========== NAND OOB Verify ==========
        OOB data check OK (16 bytes x 10 pages)

    Args:
        logs: NAND verify log lines

    Returns:
        Structured analysis result
    """
    analysis = {
        'phase_verify': {
            'modes': [],
            'passed': False,
            'summary': ''
        },
        'base_verify': {
            'operations': [],
            'data_checks': [],
            'summary': ''
        },
        'oob_verify': {
            'status': None,
            'detail': '',
            'summary': ''
        },
        'overall_status': 'unknown',
        'errors': [],
        'summary': ''
    }

    current_section = None

    for raw_line in logs:
        line = _strip_ansi(raw_line).strip()
        if not line:
            continue

        line_lower = line.lower()

        if 'nand phase verify' in line_lower:
            current_section = 'phase'
            continue
        elif 'nand base verify' in line_lower:
            current_section = 'base'
            continue
        elif 'nand oob verify' in line_lower:
            current_section = 'oob'
            continue

        if current_section == 'phase':
            if 'delay mode' in line_lower:
                mode_match = re.search(r'delay mode:\s*\[([^\]]+)\]', line)
                result_match = re.search(r'test\s+(Success|Failed)', line, re.IGNORECASE)
                if mode_match and result_match:
                    mode_name = mode_match.group(1)
                    test_result = result_match.group(1).lower()
                    passed = (test_result == 'success')
                    analysis['phase_verify']['modes'].append({
                        'mode': mode_name,
                        'result': 'PASS' if passed else 'FAIL'
                    })
                    if passed:
                        analysis['phase_verify']['passed'] = True

        elif current_section == 'base':
            perf_match = re.search(
                r'^(.+?):\s*(\d+)\s*byte,\s*(\d+)\s*us\s*->\s*(\d+)\s*KB/s', line
            )
            if perf_match:
                op_name = perf_match.group(1).strip()
                size = int(perf_match.group(2))
                time_us = int(perf_match.group(3))
                speed = int(perf_match.group(4))
                analysis['base_verify']['operations'].append({
                    'operation': op_name,
                    'size_bytes': size,
                    'time_us': time_us,
                    'speed_kbs': speed
                })
                continue

            if 'data check ok' in line_lower:
                analysis['base_verify']['data_checks'].append('OK')
            elif 'data check' in line_lower and 'failed' in line_lower:
                analysis['base_verify']['data_checks'].append('FAILED')

        elif current_section == 'oob':
            if 'oob data check ok' in line_lower:
                analysis['oob_verify']['status'] = 'OK'
                analysis['oob_verify']['detail'] = line
            elif 'oob data check' in line_lower and 'failed' in line_lower:
                analysis['oob_verify']['status'] = 'FAILED'
                analysis['oob_verify']['detail'] = line

        if 'error' in line_lower and current_section is not None:
            analysis['errors'].append(line)

    if analysis['phase_verify']['modes']:
        pass_count = sum(1 for m in analysis['phase_verify']['modes'] if m['result'] == 'PASS')
        total = len(analysis['phase_verify']['modes'])
        if analysis['phase_verify']['passed']:
            analysis['phase_verify']['summary'] = f"✅ {pass_count}/{total} delay modes passed"
        else:
            analysis['phase_verify']['summary'] = f"❌ All {total} delay modes failed"

    ops = analysis['base_verify']['operations']
    if ops:
        checks_ok = all(c == 'OK' for c in analysis['base_verify']['data_checks'])
        if analysis['base_verify']['data_checks'] and checks_ok:
            analysis['base_verify']['summary'] = f"✅ {len(ops)} operations completed, data check OK"
        elif analysis['base_verify']['data_checks']:
            analysis['base_verify']['summary'] = f"⚠️ {len(ops)} operations, data check issues"
        else:
            analysis['base_verify']['summary'] = f"✅ {len(ops)} operations completed"

    if analysis['oob_verify']['status'] == 'OK':
        analysis['oob_verify']['summary'] = "✅ OOB verify OK"
    elif analysis['oob_verify']['status'] == 'FAILED':
        analysis['oob_verify']['summary'] = "❌ OOB verify FAILED"

    has_phase = analysis['phase_verify']['passed']
    has_base = bool(ops) and all(c == 'OK' for c in analysis['base_verify']['data_checks'])
    has_oob = analysis['oob_verify']['status'] == 'OK'
    no_errors = len(analysis['errors']) == 0

    if has_phase and has_base and has_oob and no_errors:
        analysis['overall_status'] = 'pass'
        analysis['summary'] = "✅ NAND verify all passed (Phase + Base + OOB)"
    elif (not analysis['phase_verify']['modes']
          and not ops
          and analysis['oob_verify']['status'] is None):
        analysis['overall_status'] = 'unknown'
        analysis['summary'] = (
            "⚠️ No NAND verify output detected, firmware may not include nand_verify command"
        )
    elif analysis['errors']:
        analysis['overall_status'] = 'fail'
        analysis['summary'] = f"❌ NAND verify failed ({len(analysis['errors'])} errors)"
    else:
        parts = []
        if has_phase:
            parts.append("Phase ✅")
        else:
            parts.append("Phase ❌")
        if has_base:
            parts.append("Base ✅")
        else:
            parts.append("Base ❌")
        if has_oob:
            parts.append("OOB ✅")
        else:
            parts.append("OOB ❌")
        analysis['overall_status'] = 'partial'
        analysis['summary'] = f"⚠️ NAND verify partial: {', '.join(parts)}"

    return analysis


# ==================== Main Workflow ====================

def verify_nand_workflow(
    host: str = 'localhost',
    port: int = 9100,
    monitor_port: str = '',
    baudrate: int = 115200,
    timeout: int = 300
) -> Dict[str, Any]:
    """
    One-click NAND model verification workflow.

    Automatically performs:
    1. Read board model from .config
    2. Modify defconfig to enable CONFIG_AIC_NAND_VERIFY
    3. Build firmware (scons)
    4. Burn firmware (reuses burn_firmware with env detection and remote upload)
    5. Send nand_verify command
    6. Collect and analyze three-phase logs (burn + boot + nand_verify)

    Args:
        host: Server address
        port: Server port
        monitor_port: Serial log port (e.g. COM5)
        baudrate: Baud rate
        timeout: Timeout in seconds

    Returns:
        Structured result dictionary
    """
    result = {
        'success': False,
        'steps': [],
        'error': '',
        'burn_analysis': None,
        'boot_analysis': None,
        'nand_verify_analysis': None
    }

    sdk_path = os.getcwd()
    print(f"[SKILL:VERIFY] === NAND Verify Workflow Started ===")
    print(f"[SKILL:VERIFY] SDK path (cwd): {sdk_path}")
    print(f"[SKILL:VERIFY] Server: {host}:{port}")
    print(f"[SKILL:VERIFY] Monitor: {monitor_port or 'disabled'}")

    # ========== Step 1: Read board name ==========
    print(f"[SKILL:VERIFY] Step 1: Reading board config...")
    try:
        board_name = _get_board_from_config(sdk_path)
        print(f"[SKILL:VERIFY] Board: {board_name}")
        result['steps'].append(f"Board detected: {board_name}")
    except (FileNotFoundError, ValueError) as e:
        result['error'] = str(e)
        result['steps'].append("FAILED: Board detection")
        return result

    # ========== Step 2: Modify defconfig ==========
    print(f"[SKILL:VERIFY] Step 2: Enabling NAND verify in defconfig...")
    ok, msg = enable_nand_verify_in_defconfig(sdk_path, board_name)
    if not ok:
        result['error'] = f"Failed to modify defconfig: {msg}"
        result['steps'].append("FAILED: defconfig modification")
        return result
    print(f"[SKILL:VERIFY] defconfig: {msg}")
    result['steps'].append(f"defconfig: {msg}")

    # ========== Step 3: Compile firmware ==========
    print(f"[SKILL:VERIFY] Step 3: Compiling firmware...")
    ok, msg = compile_firmware(sdk_path, timeout=120)
    if not ok:
        result['error'] = msg
        result['steps'].append("FAILED: Compilation")
        return result
    print(f"[SKILL:VERIFY] {msg}")
    result['steps'].append(f"Compilation: {msg}")

    # ========== Step 4: Find compiled firmware ==========
    print(f"[SKILL:VERIFY] Step 4: Finding compiled firmware...")
    image_path = _find_latest_image(sdk_path, board_name)
    if not image_path:
        result['error'] = "No compiled firmware image found in output/"
        result['steps'].append("FAILED: Firmware not found")
        return result
    print(f"[SKILL:VERIFY] Firmware: {image_path}")
    result['steps'].append(f"Firmware: {os.path.basename(image_path)}")

    # ========== Step 5: Burn firmware ==========
    print(f"[SKILL:VERIFY] Step 5: Burning firmware...")

    if not monitor_port:
        result['error'] = (
            "Serial monitor port is required for NAND verification.\n"
            "Please configure monitor_port in aiburnconfig.json or pass --monitor parameter."
        )
        result['steps'].append("FAILED: Monitor port required")
        return result

    burn_result = burn_firmware(
        image_path=image_path,
        serial_port='',
        baudrate=baudrate,
        host=host,
        port=port,
        timeout=timeout,
        monitor_port=monitor_port
    )

    if not burn_result.get('success'):
        result['error'] = f"Burn failed: {burn_result.get('error', 'unknown')}"
        result['steps'].append("FAILED: Burn")
        result['burn_analysis'] = burn_result.get('burn_analysis')
        return result

    result['burn_analysis'] = burn_result.get('burn_analysis')
    result['boot_analysis'] = burn_result.get('boot_analysis')
    result['steps'].append("Burn: success")

    # ========== Step 6: Send nand_verify command ==========
    print(f"[SKILL:VERIFY] Step 6: Sending nand_verify command...")

    from .aicupg_client import AICUPGClient

    client = AICUPGClient(host, port)
    if not client.connect():
        result['error'] = f'Failed to connect to Server at {host}:{port}'
        result['steps'].append("FAILED: TCP connection")
        return result

    try:
        print(f"[SKILL:VERIFY] Waiting for shell prompt (3s)...")
        time.sleep(3)
        client.drain_stream_queue()

        cmd_result = client.send_serial_command("nand_verify", auto_newline=True)
        if not cmd_result:
            result['error'] = "Failed to send nand_verify command (no response from server)"
            result['steps'].append("FAILED: send nand_verify")
            return result

        if cmd_result.get('status') != 'success':
            result['error'] = f"nand_verify send failed: {cmd_result}"
            result['steps'].append("FAILED: send nand_verify")
            return result

        print(f"[SKILL:VERIFY] nand_verify command sent successfully")
        result['steps'].append("nand_verify: sent")

        # ========== Step 7: Collect NAND verify logs ==========
        print(f"[SKILL:VERIFY] Step 7: Collecting NAND verify logs (60s)...")
        nand_logs = client.collect_logs(duration_sec=60.0, stop_on_pattern="aic />")
        print(f"[SKILL:VERIFY] NAND verify logs: {len(nand_logs)} lines")

        if nand_logs:
            for i, log_line in enumerate(nand_logs[:5]):
                print(f"[SKILL:VERIFY]   [{i}] {log_line}")
            if len(nand_logs) > 5:
                print(f"[SKILL:VERIFY]   ... ({len(nand_logs) - 5} more lines)")

        result['nand_verify_logs'] = nand_logs
        result['steps'].append(f"NAND logs: {len(nand_logs)} lines")

    finally:
        client.disconnect()

    # ========== Step 8: Analyze NAND verify logs ==========
    print(f"[SKILL:VERIFY] Step 8: Analyzing NAND verify logs...")
    nand_analysis = _analyze_nand_verify_logs(nand_logs)
    result['nand_verify_analysis'] = nand_analysis
    print(f"[SKILL:VERIFY] NAND analysis: {nand_analysis['summary']}")

    # ========== Final Result ==========
    result['success'] = (nand_analysis['overall_status'] == 'pass')
    result['status'] = 'success' if result['success'] else 'failed'

    print(f"[SKILL:VERIFY] === Workflow {'SUCCESS' if result['success'] else 'FAILED'} ===")
    print(f"[SKILL:VERIFY] Summary: {nand_analysis['summary']}")

    return result
