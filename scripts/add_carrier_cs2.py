"""Add reserved GPIO4 CS2 to the EDA-exported R0, preserving existing copper.

Produces a separate review revision. Geometric checks here do not replace EDA DRC.
"""
import heapq
import json
import math
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'hardware/weather-carrier'
with zipfile.ZipFile(OUT / 'weather-carrier-r0-routed-review.epro') as z:
    files = {n: z.read(n) for n in z.namelist() if not n.endswith('/')}
pcb_name = next(n for n in files if n.endswith('.epcb'))
sch_name = next(n for n in files if n.endswith('.esch'))
pcb = [json.loads(s) for s in files[pcb_name].decode().splitlines() if s]
sch = [json.loads(s) for s in files[sch_name].decode().splitlines() if s]
for r in pcb:
    if r[0] == 'PAD_NET' and r[1:3] == ['e30', '9']:
        assert r[3] == ''
        r[3] = 'IO4'
    if r[0] == 'STRING' and r[6] == 'NC':
        r[6] = 'CS2'
for r in sch:
    if r[0] == 'TEXT':
        r[5] = r[5].replace('CS2(NC)', 'CS2(IO4)').replace(
            'Verify module VIN accepts 5V.', 'Module VIN=5V confirmed by user.')
sch.extend([
    ['WIRE', 'cs2_wire', [[350, 560, 280, 560]], 'st3', 0],
    ['ATTR', 'cs2_net', 'cs2_wire', 'NET', 'IO4', 0, 1, 280, 560, 0, 'st4', 0],
])

# Five-mil grid with conservative obstacle inflation. Cardinal grid edges
# cannot graze original obstacles because an extra grid diagonal is included.
STEP, N = 5, 631
grid = [bytearray(N*N), bytearray(N*N)]
margin = 5 + 8 + STEP * math.sqrt(2)  # half trace + clearance + grid allowance

def obstacle(layer, x1, y1, x2, y2, radius):
    dx, dy = x2-x1, y2-y1
    norm = dx*dx + dy*dy
    for iy in range(max(0, math.floor((min(y1,y2)-radius)/STEP)),
                    min(N, math.ceil((max(y1,y2)+radius)/STEP)+1)):
        for ix in range(max(0, math.floor((min(x1,x2)-radius)/STEP)),
                        min(N, math.ceil((max(x1,x2)+radius)/STEP)+1)):
            x,y=ix*STEP,iy*STEP
            t=max(0,min(1,((x-x1)*dx+(y-y1)*dy)/norm)) if norm else 0
            if (x-x1-t*dx)**2+(y-y1-t*dy)**2 <= radius*radius:
                grid[layer][iy*N+ix]=1

for r in pcb:
    if r[0]=='LINE' and r[4] in (1,2):
        obstacle(r[4]-1,r[5],-r[6],r[7],-r[8],r[9]/2+margin)
    if r[0]=='VIA':
        for layer in (0,1):
            obstacle(layer,r[5],-r[6],r[5],-r[6],r[8]/2+margin)
pads=json.loads((OUT/'connectivity.json').read_text())['pads']
for p in pads:
    if p['net']=='IO4':
        continue
    # Circumscribed circle also safely bounds rectangular SMD copper.
    radius=math.hypot(p['shape'][1],p['shape'][2])/2+margin
    for layer in ((0,1) if p['layer']==12 else (p['layer']-1,)):
        obstacle(layer,p['x'],-p['y'],p['x'],-p['y'],radius)
start=(75,233)  # J1.4 = (375, -1165.1)
goal=(210,382)  # J4.9 = (1050, -1910)
for g in grid:
    for i in range(N):
        for j in range(6):
            g[j*N+i]=g[(N-j-1)*N+i]=g[i*N+j]=g[i*N+N-j-1]=1

# Search both layers; via sites require a clear 45-mil square on both layers.
a=start[1]*N+start[0]; b=goal[1]*N+goal[0]
queue=[];prev={};costs={}
for layer in (0,1):
    node=layer*N*N+a
    if not grid[layer][a]:
        heapq.heappush(queue,(0,0,node));prev[node]=None;costs[node]=0
route=None
while queue:
    _,cost,cur=heapq.heappop(queue)
    if cost!=costs[cur]: continue
    layer,pos=divmod(cur,N*N); x,y=pos%N,pos//N
    if pos==b:
        route=[]
        while cur is not None:
            ll,pp=divmod(cur,N*N)
            route.append((pp%N*STEP,-(pp//N)*STEP,ll))
            cur=prev[cur]
        route.reverse();break
    candidates=[(xx,yy,layer,1) for xx,yy in ((x+1,y),(x-1,y),(x,y+1),(x,y-1))]
    if 5<x<N-5 and 5<y<N-5 and all(not grid[ll][yy*N+xx]
            for ll in (0,1) for xx in range(x-4,x+5) for yy in range(y-4,y+5)):
        candidates.append((x,y,1-layer,50))
    for xx,yy,ll,stepcost in candidates:
        if not (0<=xx<N and 0<=yy<N) or grid[ll][yy*N+xx]: continue
        nxt=ll*N*N+yy*N+xx; nc=cost+stepcost
        if nc>=costs.get(nxt,10**9): continue
        costs[nxt]=nc;prev[nxt]=cur
        heapq.heappush(queue,(nc+abs(xx-goal[0])+abs(yy-goal[1]),nc,nxt))
if not route:
    raise RuntimeError('No clear route; use EDA interactive routing')
simple=[route[0]]
for i in range(1,len(route)-1):
    a,b,c=route[i-1:i+2]
    if tuple(b[j]-a[j] for j in range(3))!=tuple(c[j]-b[j] for j in range(3)):
        simple.append(b)
simple.append(route[-1]);simple[0]=(375,-1165.1,simple[0][2])
for i,(a,b) in enumerate(zip(simple,simple[1:])):
    if a[2]!=b[2]:
        pcb.append(['VIA',f'cs2_via_{i}',0,'IO4','',a[0],a[1],12,24,0,None,None,0,[]])
    else:
        pcb.append(['LINE',f'cs2_route_{i}',0,'IO4',a[2]+1,*a[:2],*b[:2],10,0])
for name,rows in ((pcb_name,pcb),(sch_name,sch)):
    files[name]=('\n'.join(json.dumps(r,ensure_ascii=False,separators=(',',':')) for r in rows)+'\n').encode()
project=json.loads(files['project.json'])
project['config']['title']='ESP32-S3 AirPlay Weather Carrier R1 CS2 REVIEW'
files['project.json']=json.dumps(project,ensure_ascii=False,indent=2).encode()
dest=OUT/'weather-carrier-r1-cs2-review.epro'
with zipfile.ZipFile(dest,'w',zipfile.ZIP_DEFLATED) as z:
    for name,data in files.items():
        z.writestr(name,data)
print(f'CS2 to GPIO4: {len(simple)-1} segments, layer {layer+1}; EDA DRC pending: {dest}')
