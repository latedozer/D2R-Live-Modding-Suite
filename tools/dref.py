#!/usr/bin/env python3
# usage: dref.py <rva>...  - find rip-relative references (lea/mov/cmp...) to data addresses in .text
import sys,mmap,struct,os
f=open(os.path.join(os.path.dirname(__file__),'../bin/D2RLoader_6ab3782c_dump.exe'),'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
import numpy as np
T0,T1=0x1000,0x1cb0000
buf=np.frombuffer(m,dtype=np.uint8,count=T1)
targets=[int(a,16) for a in sys.argv[1:]]
# for each offset i in text: disp = int32 at i ; ref if i+4+disp == target
idx=np.arange(T0,T1-3,dtype=np.int64)
b=buf[T0:T1].astype(np.int64)
disp=(b[:-3]|(b[1:-2]<<8)|(b[2:-1]<<16)|(b[3:]<<24))
disp=np.where(disp>=2**31,disp-2**32,disp)
dest=idx[:len(disp)]+4+disp
for t in targets:
    hits=idx[:len(disp)][dest==t]
    print(hex(t),[hex(int(h)-3) for h in hits])
