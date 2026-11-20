#!/usr/bin/env python3
"""
AIBURN CLI - AI Agent Firmware Burn & Device Management Entry Point

Usage:
    aiburn.py burn --image firmware.img              # Burn firmware
    aiburn.py burn --image firmware.img --monitor COM6  # Burn with serial monitor
    aiburn.py readlog --monitor COM5 --duration 30    # Read device serial logs
    aiburn.py sendcmd --command "reset"                 # Send via USB
    aiburn.py sendcmd --command "free" --serial --monitor COM5  # Send via serial
    aiburn.py getstatus                               # Get device status

All parameters auto-read from aiburnconfig.json, CLI args override config.
Outputs structured JSON to stdout for Agent parsing.
"""

import argparse
import sys
import os
import json
import traceback


def _setup_package_path():
    """Auto-locate aiburn/ directory so no pip install is needed."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    pkg_dir = os.path.join(script_dir, 'aiburn')

    if os.path.isdir(pkg_dir) and pkg_dir not in sys.path:
        sys.path.insert(0, pkg_dir)
        print(f"[INFO] Added package path: {pkg_dir}", file=sys.stderr)


_setup_package_path()


def load_config():
    """Load configuration from aiburnconfig.json in current working directory."""
    config_path = os.path.join(os.getcwd(), "aiburnconfig.json")
    if not os.path.exists(config_path):
        return {}
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        print(f"[ERROR] Failed to load config: {e}", file=sys.stderr)
        return {}


def _ensure_monitor_running(client, monitor_port, baudrate):
    """Start log monitor if not already running."""
    probe = client.collect_logs(duration_sec=0.5)
    if probe:
        print("[CMD] Monitor already running", file=sys.stderr)
        return
    print(f"[CMD] Starting monitor on {monitor_port}...", file=sys.stderr)
    result = client.start_log_monitor(monitor_port, baudrate)
    if result and result.get('status') == 'success':
        print("[CMD] Monitor started", file=sys.stderr)
    else:
        print(f"[CMD] Warning: Failed to start monitor: {result}", file=sys.stderr)


def tcp_command(host, port, cmd_name, cmd_params=None, monitor_port=None, baudrate=None):
    """Send a command to TCP server and return result. Handles all error output."""
    print(f"[CMD] {cmd_name} -> {host}:{port}", file=sys.stderr)
    try:
        from aicupg_client import AICUPGClient
        client = AICUPGClient(host, port)
        if not client.connect():
            err_msg = f'Failed to connect to {host}:{port}'
            print(json.dumps({'success': False, 'error': err_msg},
                             indent=2, ensure_ascii=False))
            sys.exit(1)

        if cmd_name == 'get_status':
            devices = client.get_device_list()
            if devices is not None:
                result = {'success': True, 'devices': devices,
                          'device_count': len(devices)}
            else:
                result = {'success': False, 'error': 'No USB device found',
                          'devices': [], 'device_count': 0}
        elif cmd_name == 'shell':
            command = cmd_params.get('cmd', '') if cmd_params else ''
            use_serial = cmd_params.get('serial', False)

            if use_serial:
                if monitor_port:
                    _ensure_monitor_running(client, monitor_port, baudrate)
                resp = client.send_serial_command(command)
                if resp and resp.get('status') == 'success':
                    logs = client.collect_logs(duration_sec=2.0)
                    output = '\n'.join(logs) if logs else '(no output)'
                    result = {'success': True, 'output': output}
                else:
                    err = resp.get('error', 'Failed to send command') if resp else 'No response'
                    result = {'success': False, 'error': err}
            else:
                devices = client.get_device_list()
                if not devices or len(devices) == 0:
                    result = {'success': False, 'error': 'No USB device found'}
                else:
                    dev_port = devices[0].get('port')
                    if not dev_port:
                        result = {'success': False, 'error': 'USB device port is None'}
                    else:
                        print(f"[USB] Configuring device: {dev_port}", file=sys.stderr)
                        cfg_resp = client.config_device(dev_port, baudrate=0)
                    if cfg_resp and cfg_resp.get('status') == 'success':
                        resp = client.send_cmd(command)
                        if resp and resp.get('status') == 'success':
                            result = {'success': True, 'output': resp.get('data', '')}
                        else:
                            err = resp.get('error', 'Command failed') if resp else 'No response'
                            result = {'success': False, 'error': err}
                    else:
                        err = (cfg_resp.get('error', 'Config device failed')
                               if cfg_resp else 'No response')
                        result = {'success': False, 'error': err}
        elif cmd_name == 'get_log':
            duration = cmd_params.get('duration', 30) if cmd_params else 30

            if monitor_port:
                _ensure_monitor_running(client, monitor_port, baudrate)

            logs = client.collect_logs(duration_sec=float(duration))
            if logs:
                result = {'success': True, 'log': '\n'.join(logs), 'source': 'serial'}
            else:
                msg = 'No serial log available (Monitor may not be running or no output)'
                result = {'success': False, 'error': msg}

        client.disconnect()
        print(json.dumps(result, indent=2, ensure_ascii=False))
        sys.exit(0 if result.get('success') else 1)
    except ImportError as e:
        print(f"[ERROR] aicupg_client not available: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(json.dumps({'success': False, 'error': str(e)}, indent=2, ensure_ascii=False))
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)


def run_burn(args, config):
    """Handle burn workflow."""
    device_cfg = config.get("device", {})

    host = args.host or config.get('server', {}).get('host', 'localhost')
    port = args.port or config.get('server', {}).get('port', 9100)

    common = {
        'host': host,
        'port': port,
        'monitor_port': args.monitor or device_cfg.get('monitor_port', ''),
        'baudrate': args.baudrate or device_cfg.get('baudrate', 115200),
        'timeout': args.timeout
    }

    kwargs = {'image_path': args.image, **common}
    print(f"[MODE] Burn: {args.image}", file=sys.stderr)
    for k, v in kwargs.items():
        if k != 'image_path':
            print(f"  {k}: {v}", file=sys.stderr)

    try:
        from aiburn import burn_firmware
    except ImportError as e:
        print(f"[ERROR] aiburn not available: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        result = burn_firmware(**kwargs)
    except Exception as e:
        print(json.dumps({
            'success': False, 'status': 'error', 'exit_code': -1,
            'error': str(e), 'burn_analysis': None, 'boot_analysis': None
        }, indent=2, default=str, ensure_ascii=False))
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)

    summary = {
        'success': result.get('success', False),
        'status': result.get('status', 'unknown'),
        'exit_code': result.get('exit_code', -1),
        'duration_ms': result.get('duration_ms', 0),
        'summary': result.get('summary', ''),
        'error': result.get('error', ''),
        'burn_logs_count': len(result.get('burn_logs', [])),
        'boot_logs_count': len(result.get('boot_logs', [])),
        'burn_analysis': result.get('burn_analysis'),
        'boot_analysis': result.get('boot_analysis')
    }
    print(json.dumps(summary, indent=2, default=str, ensure_ascii=False))

    if result.get('success'):
        print(f"\n✅ Burn completed ({result.get('duration_ms', 0)}ms)", file=sys.stderr)
    else:
        print(f"\n❌ Burn failed", file=sys.stderr)
        if result.get('error'):
            print(f"   {result['error']}", file=sys.stderr)

    sys.exit(0 if result.get('success') else 1)


def dispatch(args, config):
    """Route to the appropriate handler based on subcommand."""
    server_cfg = config.get('server', {})
    host = args.host or server_cfg.get('host', 'localhost')
    port = args.port or server_cfg.get('port', 9100)

    device_cfg = config.get('device', {})
    monitor_port = getattr(args, 'monitor', None) or device_cfg.get('monitor_port', '')
    baudrate = getattr(args, 'baudrate', None) or device_cfg.get('baudrate', 115200)

    if args.command == 'readlog':
        tcp_command(host, port, 'get_log', {'duration': args.duration},
                    monitor_port=monitor_port, baudrate=baudrate)
    elif args.command == 'sendcmd':
        tcp_command(host, port, 'shell',
                    {'cmd': args.shell_cmd, 'serial': getattr(args, 'serial', False)},
                    monitor_port=monitor_port, baudrate=baudrate)
    elif args.command == 'getstatus':
        tcp_command(host, port, 'get_status')
    elif args.command == 'burn':
        run_burn(args, config)


def main():
    parser = argparse.ArgumentParser(
        description='AIBURN CLI - Firmware burn & device management',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  aiburn.py burn --image firmware.img
  aiburn.py burn --image firmware.img --monitor COM6
  aiburn.py readlog --duration 60
  aiburn.py sendcmd --command "reset"
  aiburn.py sendcmd --command "free" --serial
  aiburn.py getstatus
        """
    )
    common_parser = argparse.ArgumentParser(add_help=False)
    common_parser.add_argument('--host', default=None, help='Server host')
    common_parser.add_argument('--port', type=int, default=None, help='Server port')
    common_parser.add_argument('--timeout', type=int, default=300,
                               help='Timeout in seconds (default: 300)')

    device_parser = argparse.ArgumentParser(add_help=False)
    device_parser.add_argument('--monitor', default=None, help='Serial port (e.g., COM5)')
    device_parser.add_argument('--baudrate', type=int, default=None, help='Baudrate')

    subparsers = parser.add_subparsers(dest='command', help='Available commands')

    burn_parser = subparsers.add_parser('burn', parents=[common_parser, device_parser],
                                        help='Burn firmware image')
    burn_parser.add_argument('--image', required=True, help='Firmware image path')

    log_parser = subparsers.add_parser('readlog', parents=[common_parser, device_parser],
                                       help='Read serial logs (auto-starts monitor)')
    log_parser.add_argument('--duration', type=int, default=30,
                            help='Log collection time in seconds (default: 30)')

    cmd_parser = subparsers.add_parser('sendcmd', parents=[common_parser, device_parser],
                                       help='Send shell command to device')
    cmd_parser.add_argument('--command', dest='shell_cmd', required=True,
                            help='Command to execute')
    cmd_parser.add_argument('--serial', action='store_true',
                            help='Send via serial port')

    subparsers.add_parser('getstatus', parents=[common_parser],
                          help='Get device status')

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        sys.exit(0)

    config = load_config()
    dispatch(args, config)


if __name__ == '__main__':
    main()
