#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
# usage: xref.py <rva>...  - find direct call/jmp rel32 references in .text of the image dump
import sys,mmap,struct,os
f=open(os.path.join(os.path.dirname(__file__),'../bin/D2RLoader_6ab3782c_dump.exe'),'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
T0,T1=0x1000,0x1cb0000
for a in sys.argv[1:]:
    t=int(a,16); out=[]
    for op in (b'\xe8',b'\xe9'):
        i=m.find(op,T0,T1)
        while i!=-1:
            if i+5+struct.unpack_from('<i',m,i+1)[0]==t: out.append((i,op[0]))
            i=m.find(op,i+1,T1)
    print(hex(t),' '.join(('call:' if o==0xe8 else 'jmp:')+hex(i) for i,o in sorted(out)))
