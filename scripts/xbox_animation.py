"""Xbox 1167E0/116D30 key codec and local-matrix animation sampling.

The packed quaternion has four 12-bit components and half-float position/scale.
The signed divisions in the original decoder are intentional and observable.
"""
import math,struct
import numpy as np

# Original reciprocal-square-root seed table, used only by the offline exporter.
_RSQRT=bytes.fromhex('69686665646261605e5d5c5b5958575654535251504f4e4d4b4a494847464544434241403f3e3d3c3b3a3939383736353433323131302f2e2d2c2c2b2a29292827262525242322222120201f1e1d1d1c1b1b1a191918171716151514141312121111100f0f0e0e0d0c0c0b0b0a0a090808070706060505040403030202010100fffdfbf9f7f5f3f1f0eeeceae8e7e5e3e2e0dedddbdad8d6d5d3d2d0cfcdcccac9c8c6c5c3c2c1bfbebdbbbab9b8b6b5b4b3b1b0afaeadabaaa9a8a7a6a5a3a2a1a09f9e9d9c9b9a999897969594939291908f8e8d8c8b8a8989888786858483828181807f7e7d7c7c7b7a797878777675747473727171706f6e6e6d6c6c6b6a')

def xbox_length(vector):
 square=np.float32(sum(float(x)*float(x) for x in vector))
 if not square>0:return 0.
 bits=struct.unpack('<I',struct.pack('<f',square))[0]
 seed=((0xbe7fffff-bits)&0xffffffff)//2 & 0xff800000
 seed|=_RSQRT[(bits>>16)&255]<<15
 y=struct.unpack('<f',struct.pack('<I',seed))[0]
 y=np.float32(1.5*y-.5*float(square)*y*y*y)
 return float(np.float32(float(square)*float(y)))

def track_scale(track):
 if not hasattr(track,'_xbox_scale'):
  track._xbox_scale=np.array([xbox_length(row) for row in track.keyframes[0].transform[:3]])
  if min(abs(track._xbox_scale))<1e-10:track._xbox_scale=np.ones(3)
 return track._xbox_scale

def half_bits(x):
 v=struct.unpack('<I',struct.pack('<f',x))[0];e=((v>>23)&255)-127;m=((v&0x7fffff)+0x1000)>>13;sign=(v>>16)&0x8000
 if e>15:return (sign+m+0x7c00)&65535
 if e<=-15:return (sign+((m+0x400)>>min(31,-14-e)))&65535
 return (sign+((e+15)<<10)+m)&65535

def half(x):return struct.unpack('<e',struct.pack('<H',half_bits(x)))[0]

def quaternion(m):
 # Original 116450 uses row-vector matrices and stores w,x,y,z.
 tr=float(np.trace(m));q=np.zeros(4)
 if tr>0:
  length=math.sqrt(tr+1);q[0]=.5*length;factor=.5/length
  q[1:]=[(m[1,2]-m[2,1])*factor,(m[2,0]-m[0,2])*factor,(m[0,1]-m[1,0])*factor]
 else:
  i=0 if m[0,0]>=m[1,1] else 1
  if m[i,i]<=m[2,2]:i=2
  j=(i+1)%3;k=(j+1)%3;length=math.sqrt(m[i,i]-m[j,j]-m[k,k]+1);factor=.5/length if length else 0
  q[i+1]=.5*length;q[0]=(m[j,k]-m[k,j])*factor;q[j+1]=(m[i,j]+m[j,i])*factor;q[k+1]=(m[i,k]+m[k,i])*factor
 return q.astype('f4')

def encode(transform):
 m=np.array(transform,dtype='f8');norms=np.linalg.norm(m[:3],axis=1);rotation=m[:3]/norms[:,None]
 q=quaternion(rotation);ints=[int(float(x)*2047) for x in q]
 packed=(ints[0]+2048)|((ints[1]+2048)<<12)|((ints[2]+2048)<<24)|((ints[3]+2048)<<36)
 return packed.to_bytes(6,'little')+struct.pack('<4H',*(half_bits(x) for x in [*m[3],sum(norms)/3]))

def decode(key):
 a=int.from_bytes(key[:4],'little',signed=True);b=int.from_bytes(key[3:7],'little',signed=True)
 ints=[(a&4095)-2048,(int(a/4096)&4095)-2048,(b&4095)-2048,(int(b/4096)&4095)-2048]
 qw,x,y,z=np.array(ints,dtype='f8')*float(np.float32(1/2047));s=2/(qw*qw+x*x+y*y+z*z)
 col=np.array([[1-s*(y*y+z*z),s*(x*y-z*qw),s*(x*z+y*qw)], [s*(x*y+z*qw),1-s*(x*x+z*z),s*(y*z-x*qw)],[s*(x*z-y*qw),s*(y*z+x*qw),1-s*(x*x+y*y)]])
 pos=struct.unpack('<4e',key[6:14]);m=np.eye(4);m[:3,:3]=col.T*pos[3];m[3,:3]=pos[:3];return m

def key(transform):return decode(encode(transform))

def ortho(m):
 out=m.copy()
 for i in range(3):
  for j in range(i):out[i,:3]-=np.dot(out[i,:3],out[j,:3])*out[j,:3]
  norm=np.linalg.norm(out[i,:3]);out[i,:3]/=norm
 return out

def sample_raw(track,frame):
 keys=track.keyframes
 if not hasattr(track,'_xbox_matrices'):track._xbox_matrices=[key(k.transform) for k in keys]
 decoded=track._xbox_matrices
 for i,(a,b) in enumerate(zip(keys,keys[1:])):
  if frame<=b.frame_id:
   t=np.clip((frame-a.frame_id)/(b.frame_id-a.frame_id),0,1);m=decoded[i]*(1-t)+decoded[i+1]*t;break
 else:m=decoded[-1].copy()
 m[:3,:3]*=track_scale(track)[:,None]
 return m

def normalization(track):
 mode=track.frame_to_frame_interpolation_method
 if mode not in (0,1,2):raise ValueError('Unknown Xbox temporal normalization')
 # The sole maintain-scaling human clip has unit scale at every source key.
 # Reject a new non-unit source rather than silently approximating that path.
 if track.maintain_matrix_scaling:
  if any(abs(np.linalg.norm(key(k.transform)[:3,:3],axis=1)-1).max()>1e-6 for k in track.keyframes):
   raise ValueError('Non-unit maintained animation scaling needs an explicit scale track')
 return mode | (4 if track.maintain_matrix_scaling else 0)

def sample(track,frame):
 m=sample_raw(track,frame);flags=normalization(track);mode=flags&3
 if mode==1:m[:3,:3]/=np.linalg.norm(m[:3,:3],axis=1)[:,None]
 if mode==2:m=ortho(m)
 if flags&4:m[:3,:3]*=track_scale(track)[:,None]
 return m
