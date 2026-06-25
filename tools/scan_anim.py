import re
from pathlib import Path
p=Path('include/matrix16x8.h')
s=p.read_text()
m=re.search(r"anim\[\]\s*=\s*\{([\s\S]*?)\};",s)
if not m:
    print('anim not found')
    raise SystemExit(1)
nums=re.findall(r"0x[0-9A-Fa-f]+|\d+",m.group(1))
data=[int(x,0) for x in nums]
# parse
p=0
blocks=[]
idx=0
while p+1 < len(data):
    a=data[p]
    if a>=0x90:
        p+=1
        idx+=1
        continue
    b=data[p+1]
    x1=a>>4; y1=a&0x0F
    x2=b>>4; y2=b&0x0F
    if x2<x1 or y2<y1 or x1>15 or x2>15 or y1>15 or y2>15:
        # malformed header, stop
        break
    count=(x2-x1+1)*(y2-y1+1)
    payload=data[p+2:p+2+count]
    if len(payload)<count:
        break
    blocks.append((p,x1,y1,x2,y2,count,payload))
    p += 2+count
    idx+=1

print(f'total bytes: {len(data)}, blocks parsed: {len(blocks)}')
print('Blocks that include x=8:')
for i,(pos,x1,y1,x2,y2,count,payload) in enumerate(blocks):
    if x1<=8<=x2:
        print(f'block#{i} at byte {pos}: x1={x1} y1={y1} x2={x2} y2={y2} count={count} payload_sample={payload[:min(8,len(payload))]}')

# Also report whether any block covers x=8 across dataset
found=any(x1<=8<=x2 for (_,x1,y1,x2,y2,_,_) in blocks)
print('\nFound blocks covering x=8:', found)

# Quick summary of x ranges distribution
from collections import Counter
ctr=Counter()
for (_,x1,_,x2,_,_,_) in blocks:
    for x in range(x1,x2+1):
        ctr[x]+=1
print('\nColumns coverage (x:count blocks):')
for x in sorted(ctr.keys()):
    print(x, ctr[x])
