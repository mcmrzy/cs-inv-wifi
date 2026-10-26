"""Read-only USB inventory. Enumeration never opens a serial port."""

class Inventory:
 def __init__(self):
  self.devices={};self.revisions={};self.pending=set()
 def epoch(self,port):return self.revisions.get(port,0)
 def update(self,ports,selected='',busy=False):
  ports=list(ports)
  current={p.device:(p.vid,p.pid,getattr(p,'serial_number',None),getattr(p,'location',None),getattr(p,'hwid',None)) for p in ports}
  changed={name for name,value in current.items() if self.devices.get(name)!=value}
  invalidated={name for name,value in self.devices.items() if current.get(name)!=value}
  for name in changed|invalidated:self.revisions[name]=self.epoch(name)+1
  candidates={p.device for p in ports if p.vid==0x303A and p.pid==0x1001}
  self.pending.intersection_update(candidates);self.pending.update(changed&candidates)
  self.devices=current;probe=None
  if not busy and len(candidates)==1:
   candidate=next(iter(candidates))
   if candidate in self.pending and (not selected or selected==candidate or selected not in current):
    probe=candidate;self.pending.remove(candidate)
  return {'ports':ports,'invalidated':invalidated,'inspect':probe,'multiple':len(candidates)>1}
 def attempted(self,port):self.pending.discard(port)
