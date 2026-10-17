from model import *
def retarget(a,m,si):
 pos,norm,uv,bi,bw,pr=a.geometry(si);newpos=pos.copy();newnorm=norm.copy();seen={};mapping=list(range(13))+list(range(14,26))
 for _,_,mode,bones,idx in pr:
  for vi in np.unique(idx):
   if mode in (0,1):newpos[vi]=pos[vi]*.62
   else:
    weights=bw[vi].copy();total=weights.sum();assert total>0;weights/=total;out=np.zeros(3);normal=np.zeros(3)
    for b,w in zip(bi[vi],weights):
     if w==0:continue
     bone=bones[b];transform=m.world[mapping[bone]]@np.diag([.62,.62,.62,1])@np.linalg.inv(a.world[bone])
     out+=(transform@np.r_[pos[vi],1])[:3]*w;normal+=(transform[:3,:3]@norm[vi])*w
    if int(vi) in seen:assert np.allclose(seen[int(vi)],out,atol=.001)
    seen[int(vi)]=out;newpos[vi]=out;length=np.linalg.norm(normal);newnorm[vi]=normal/length if length else normal
 return newpos,newnorm

def hand_plan(a,m):
 candidates={}
 for mesh in a.meshes:
  si,mat,vis=struct.unpack_from('<HBB',mesh)
  if mat!=22:continue
  bones=set(b for *_,bs,idx in a.primitives(a.shapes[si]) for b in bs)
  if bones<={15,16}:side='left'
  elif bones<={19,20}:side='right'
  else:continue
  p,n=retarget(a,m,si);candidates[si]=(side,p)
 plan={};removed=set();report=[]
 for vis in sorted(set(me[3] for me in m.meshes)):
  skin=[];ids=[];bones=set()
  for i,mesh in enumerate(m.meshes):
   si,ma,v=struct.unpack_from('<HBB',mesh)
   if v!=vis or ma not in (2,3):continue
   bb=set(b for *_,bs,idx in m.primitives(m.shapes[si]) for b in bs)
   if not (bb<={16,17} or bb<={20,21}):continue
   skin.append(m.world_geometry(si)[0]);bones|=bb;ids.append(i)
  if not skin:continue
  side='left' if bones<={16,17} else 'right';points=np.concatenate(skin);points=np.unique(points,axis=0)
  scores=[]
  for si,(s,p) in candidates.items():
   if s!=side:continue
   dist=((points[:,None,:]-p[None,:,:])**2).sum(2)
   score=float(dist.min(0).mean()+dist.min(1).mean());scores.append((score,si))
  score,si=min(scores);plan.setdefault(si,[]).append(vis);removed.update(ids);report.append({'native_group':vis,'adult_shape':si,'fit_rms':math.sqrt(score/2),'side':side})
 return plan,removed,report
if __name__=='__main__':
 import json
 r=Path(__file__).resolve().parent;a=Model((r/'oot/actor/zelda_link_boy_new/link_v2.cmb').read_bytes());m=Model((r/'mm/actors/zelda2_link_child_new/link_child.cmb').read_bytes());p,removed,report=hand_plan(a,m);print(json.dumps(report,indent=2))
