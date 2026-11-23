#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# usage: dis.sh <rva> <len>  - disassemble the image dump (file offset == RVA)
objdump -D -b binary -m i386:x86-64 -M intel --start-address=$1 --stop-address=$(($1+$2)) "$(dirname "$0")/../bin/D2RLoader_6ab3782c_dump.exe" | tail -n +8 | cut -f1,3
