import json,os,time,struct,sys
RPC=os.path.join(os.path.dirname(os.path.abspath(__file__)),'rpc')
def read(reqs):
    try: os.remove(RPC+'/resp.json')
    except OSError: pass
    json.dump(reqs,open(RPC+'/req.tmp','w')); os.replace(RPC+'/req.tmp',RPC+'/req.json')
    for _ in range(200):
        if os.path.exists(RPC+'/resp.json'):
            time.sleep(0.02); r=json.load(open(RPC+'/resp.json')); return {int(k):(bytes.fromhex(v) if v else None) for k,v in r.items()}
        time.sleep(0.02)
    raise SystemExit('no response')
B=0x140000000
