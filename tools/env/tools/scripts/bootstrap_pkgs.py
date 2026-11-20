#!/usr/bin/env python3
# -*- coding:utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2025-2026 ArtInChip Technology Co., Ltd

"""
Luban-Lite SDK – Windows Python38 offline dependency bootstrap.

Invoked synchronously by CmdInit.cmd each time ConEmu starts.
Checks whether required packages are importable in the bundled Python38;
if not, installs them from tools/env/local_pkgs/.

Wheels are extracted directly via the 'zipfile' module (no pip required),
because the bundled Python38 ships an incomplete pip installation.
For SCons a lightweight 'scons.bat' launcher is also written to Scripts/
so the 'scons' shell command works immediately after installation.

This mirrors the Linux 'pip_install_pkg()' logic in tools/scripts/oneclick.sh.
"""

import importlib
import os
import sys
import zipfile

# Each entry: (module_name, wheel_prefix_in_local_pkgs, launcher_list)
#   module_name   – passed to importlib.import_module() to test installation
#   wheel_prefix  – used to locate <prefix>*.whl inside local_pkgs/
#   launcher_list – [(bat_stem, dash_m_module), ...]; a .bat wrapper is written
#                   to Scripts/ for each entry (empty list = library only)
PACKAGES = [
    ("SCons", "scons",
     [("scons",    "SCons"),
      ("sconsign", "SCons.Utilities.sconsign")]),
    ("kconfiglib", "kconfiglib", []),  # imported as a library, no launcher
]


# ── path helpers ──────────────────────────────────────────────────────────────

def _local_pkgs_dir():
    """Return the absolute path to tools/env/local_pkgs/."""
    env_root = os.environ.get("ENV_ROOT")
    if env_root:
        return os.path.join(env_root, "local_pkgs")
    # Fallback: derive from this script location  (scripts/ -> tools/ -> env/)
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, os.pardir, os.pardir, "local_pkgs"))


def _find_wheel(pkgs_dir, prefix):
    """Return the path of the first .whl whose filename starts with *prefix*."""
    try:
        for name in sorted(os.listdir(pkgs_dir)):
            if name.startswith(prefix) and name.endswith(".whl"):
                return os.path.join(pkgs_dir, name)
    except OSError:
        pass
    return None


def _python_root():
    """Directory that contains python.exe (e.g. …/Python38/)."""
    return os.path.dirname(os.path.realpath(sys.executable))


def _site_packages():
    return os.path.join(_python_root(), "Lib", "site-packages")


def _scripts_dir():
    return os.path.join(_python_root(), "Scripts")


# ── installation helpers ──────────────────────────────────────────────────────

def _extract_wheel(wheel_path, dest):
    """Extract *wheel_path* (a ZIP archive) into *dest*.

    Directory entries are skipped; only files are written.
    Existing files are overwritten, making repeated runs idempotent.
    A simple guard rejects any path-traversal entries.
    """
    dest_norm = os.path.normpath(dest)
    with zipfile.ZipFile(wheel_path) as zf:
        for member in zf.infolist():
            if member.is_dir():
                continue
            target = os.path.normpath(os.path.join(dest, member.filename))
            if not target.startswith(dest_norm):
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with zf.open(member) as src, open(target, "wb") as dst:
                dst.write(src.read())


def _write_bat_launcher(scripts_dir, name, dash_m_module):
    """Write Scripts/<name>.bat if it does not already exist.

    The generated batch forwards all arguments to
    'python.exe -m <dash_m_module>', which is functionally equivalent to
    the console_script entry point pip would have created:
      SCons/__main__.py  →  SCons.Script.main()  ==  entry point 'scons'
    """
    bat_path = os.path.join(scripts_dir, name + ".bat")
    if os.path.exists(bat_path):
        return  # never overwrite an existing launcher (may be pip-created)
    content = (
        "@echo off\r\n"
        '"%~dp0..\\python.exe" -m {} %*\r\n'.format(dash_m_module)
    )
    os.makedirs(scripts_dir, exist_ok=True)
    with open(bat_path, "w", newline="") as fh:
        fh.write(content)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    pkgs_dir = _local_pkgs_dir()
    if not os.path.isdir(pkgs_dir):
        return  # local_pkgs not present – nothing to do

    sp = _site_packages()
    sd = _scripts_dir()
    any_installed = False

    for module_name, prefix, launchers in PACKAGES:
        # Fast path: already importable → skip entirely
        try:
            importlib.import_module(module_name)
            continue
        except ImportError:
            pass

        whl = _find_wheel(pkgs_dir, prefix)
        if whl is None:
            # Wheel not shipped; aic_build.py will raise a clear ImportError.
            continue

        sys.stdout.write("[Luban-Lite Env] Installing {} … ".format(os.path.basename(whl)))
        sys.stdout.flush()
        try:
            _extract_wheel(whl, sp)
            for bat_name, bat_module in launchers:
                _write_bat_launcher(sd, bat_name, bat_module)
            print("OK")
            any_installed = True
        except Exception as exc:  # noqa: BLE001
            print("FAILED: {}".format(exc))

    if any_installed:
        # Make the newly installed package visible to *this* interpreter as
        # well (subsequent python.exe invocations will find it automatically
        # via the standard site-packages search path).
        if sp not in sys.path:
            sys.path.insert(1, sp)


if __name__ == "__main__":
    main()
