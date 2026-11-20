# -*- coding:utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021-2026 ArtInChip Technology Co., Ltd
#
"""Allow ``python -m itbimage`` invocation."""
import sys

from itbimage._core import main
from itbimage._cipher import CipherError

if __name__ == '__main__':
    try:
        main()
    except CipherError as exc:
        print("Error: {}".format(exc))
        sys.exit(1)
