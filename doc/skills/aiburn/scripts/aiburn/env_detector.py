"""
env_detector.py - Agent Runtime Environment Auto-Detection

Detects Agent runtime environment (local Windows vs remote Linux/macOS),
to determine whether firmware file transfer via TCP is needed.

Core logic:
- Server always runs on local Windows
- Agent may be local (Windows) or remote (Linux/macOS)
- Remote Agent: firmware path is remote (e.g. /home/...), must transfer to Windows
- Local Agent: firmware path is local (e.g. C:\\...), use directly

Usage:
    from aiburn.env_detector import detect_agent_environment

    env = detect_agent_environment()
    if env['is_remote']:
        # Remote agent, firmware transfer needed
        print(f"Remote agent detected, OS: {env['agent_os']}")
        print(f"Firmware needs to be transferred via TCP")
    else:
        # Local agent, use local path directly
        print("Local agent detected")
"""

import platform


def detect_os() -> str:
    """
    Detect operating system type.

    Returns:
        'windows', 'linux', or 'darwin' (macOS)
    """
    system = platform.system().lower()
    return system if system in ('windows', 'linux', 'darwin') else 'unknown'


def detect_agent_environment(server_ip: str = 'localhost', server_os: str = 'windows') -> dict:
    """
    Detect Agent runtime environment to determine if firmware transfer is needed.

    Core logic: determines remote/local based on IP address, not OS type.

    Args:
        server_ip: Server IP address (default 'localhost')
        server_os: Server OS (kept for backward compatibility, not used for detection)

    Returns:
        Environment info dict:
        {
            'agent_os': Agent OS ('windows', 'linux', 'darwin')
            'server_ip': Server IP address
            'is_remote': Whether Agent is remote (True=transfer needed, False=local path OK)
            'path_format': Path format ('windows' or 'unix')
            'needs_transfer': Whether TCP firmware transfer is needed
            'description': Environment description string
        }
    """
    import socket

    agent_os = detect_os()

    if server_ip in ['localhost', '127.0.0.1', '::1']:
        is_remote = False
    else:
        try:
            local_ips = socket.gethostbyname_ex(socket.gethostname())[2]
            is_remote = (server_ip not in local_ips)
        except Exception as e:
            print(f"[ENV_DETECTOR] Failed to get local IPs: {e}")
            is_remote = True

    path_format = 'windows' if agent_os == 'windows' else 'unix'

    env = {
        'agent_os': agent_os,
        'server_ip': server_ip,
        'is_remote': is_remote,
        'path_format': path_format,
        'needs_transfer': is_remote,
        'description': _get_env_description(agent_os, server_ip, is_remote)
    }

    return env


def _get_env_description(agent_os: str, server_ip: str, is_remote: bool) -> str:
    """Get environment description string."""
    if not is_remote:
        return (
            f"Local {agent_os.capitalize()} agent → Local server at {server_ip} "
            f"(no transfer needed)"
        )
    else:
        return (
            f"Remote {agent_os.capitalize()} agent → Server at {server_ip} "
            f"(firmware transfer required)"
        )


def print_environment_info(server_ip: str = 'localhost'):
    """Print environment info to console (for CLI use)."""
    env = detect_agent_environment(server_ip=server_ip)

    print("=" * 70)
    print("AIBURN Agent Environment Detection")
    print("=" * 70)
    print(f"Agent OS:     {env['agent_os'].capitalize()}")
    print(f"Server IP:    {env['server_ip']}")
    print(f"Is Remote:    {'Yes' if env['is_remote'] else 'No'}")
    print(f"Path Format:  {env['path_format'].capitalize()}")
    transfer_msg = 'Yes (TCP upload required)' if env['needs_transfer'] else 'No (local path OK)'
    print(f"Needs Transfer: {transfer_msg}")
    print()
    print(f"Description:  {env['description']}")
    print("=" * 70)

    if env['needs_transfer']:
        print()
        print("⚠️  WARNING: Firmware file must be transferred via TCP before burning!")
        print("   Use: client.upload_firmware(local_path)")
    else:
        print()
        print("✓ Firmware can be burned directly using local path.")
    print("=" * 70)


if __name__ == '__main__':
    print_environment_info()
