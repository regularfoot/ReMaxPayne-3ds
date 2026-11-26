"""Read the paired exits and interaction points from the PAL Tutorial level."""
from io import BytesIO
import math
import re
import struct
from max_payne_sdk.max_type import parseType

ROOMS = ('StartRoom', 'Alley2', 'Street', 'Subway', 'Alley', 'Dead_End')
IDS = (8, 1, 6, 5, 4, 7)
EXITS = (('Exit0',), ('Exit1', 'Exit43'), ('Exit2', 'Exit5', 'Exit0', 'Exit1'),
         ('Exit4',), ('Exit3',), ('Exit0',))
USES = (('Use_01','T_Van','lock'), ('Use_03','Use_04',None),
        ('Use_04','T_Dispenser',None), (None,'t_health',None), (None,None,None), (None,None,None))


HOUSE_ROOMS = ('startroom', 'entrance03', 'entrance05', 'entrance06')
HOUSE_IDS = (5, 7, 8, 9)
HOUSE_EXITS = (('Exit0',), ('Exit2', 'Exit0'), ('Exit3', 'Exit0'), ('Exit1',))
HOUSE_MARKERS = ((None, 'note_01_trigger01', 't_hell'), ('note_01_trigger', None, None),
                 (None, None, None), (None, 'cin_trigger2', None))


def portal(data, name, room_id, rooms, ids, first_room):
    # Each exit also occurs as another exit's destination. Only its definition
    # has the tagged polygon vertex count and a vector immediately after it.
    for m in re.finditer(re.escape(name.encode()),data):
        if data[m.end()] not in (17,20) or data[m.end()+2]!=22:
            continue
        stream=BytesIO(data)
        stream.seek(m.end())
        vertices=[parseType(stream) for _ in range(parseType(stream))]
        normal, transform, source, target, other=[parseType(stream) for _ in range(5)]
        if source!=room_id or target not in ids or not other.startswith('::'+rooms[ids.index(target)]+'::'):
            continue
        span=list(zip(*vertices))
        center=((min(span[0])+max(span[0]))/2,min(span[1]),(min(span[2])+max(span[2]))/2)
        halfwidth=max(math.hypot(v[0]-center[0],v[2]-center[2]) for v in vertices)
        return struct.pack('<10fi',*center,*normal,*transform[3],halfwidth,first_room+ids.index(target))
    raise ValueError('Missing exit '+name)


def packed_exits(data, prefix, names, index, rooms, ids, first_room):
    exits = [portal(data, prefix+n, ids[index], rooms, ids, first_room) for n in names]
    return b''.join(exits + [struct.pack('<10fi', *([0]*10), -1)] * (4-len(exits)))


def house_markers(data, marker, index):
    prefix = '::'+HOUSE_ROOMS[index]+'::'
    points = [marker(data, prefix+n)[0][9:] if n else (0,0,0) for n in HOUSE_MARKERS[index]]
    exits = packed_exits(data, prefix, HOUSE_EXITS[index], index, HOUSE_ROOMS, HOUSE_IDS, len(ROOMS))
    return b''.join(struct.pack('<3f', *p) for p in points) + bytes(24) + exits + bytes(11*12)


def markers(data,marker,index):
    prefix='::'+ROOMS[index]+'::'
    names=list(USES[index])+(['Tele_02','Tele_03'] if index==2 else [None,None])
    points=[marker(data,prefix+n)[0][9:] if n else (0,0,0) for n in names]
    exits=packed_exits(data,prefix,EXITS[index],index,ROOMS,IDS,0)
    rifle = ('T_01','W03','W04','W05','P01','P02','P03')
    street = [marker(data,prefix+n)[0][9:] for n in rifle] if index==2 else [(0,0,0)]*len(rifle)
    # The first two street spawns are shared with the first fight. The remaining
    # dispenser markers are stored in the local coordinates of their own rooms.
    practice = ('::Street::Tele_05','::Subway::Tele_01','::Alley::Tele_04','::Dead_End::Tele_03')
    street.extend(marker(data,n)[0][9:] if index==2 else (0,0,0) for n in practice)
    return b''.join(struct.pack('<3f',*p) for p in points)+exits+b''.join(struct.pack('<3f',*p) for p in street)


def navigation(collision, dest):
    """Rasterize the Street walkable floor and obstructions for enemy navigation."""
    import numpy as np
    faces=np.frombuffer(collision,dtype='<f4').reshape(-1,10)
    triangles=faces[:,:9].reshape(-1,3,3)
    lo=np.floor(triangles.min(axis=(0,1))[[0,2]]).astype(int)-1
    hi=np.ceil(triangles.max(axis=(0,1))[[0,2]]).astype(int)+1
    w,h=hi-lo+1
    floor=np.full((h,w),np.nan)
    def area(v):
        xmin,zmin=np.maximum(lo,np.floor(v[:,[0,2]].min(axis=0)-.35)).astype(int)
        xmax,zmax=np.minimum(hi,np.ceil(v[:,[0,2]].max(axis=0)+.35)).astype(int)
        z,x=np.mgrid[zmin:zmax+1,xmin:xmax+1]
        return x,z,np.s_[zmin-lo[1]:zmax-lo[1]+1,xmin-lo[0]:xmax-lo[0]+1]
    for v in triangles[faces[:,9]>.62]:
        if v[:,1].min()>-2 or v[:,1].max()<-5.5: continue
        x,z,sl=area(v);a,b,c=v;b=b-a;c=c-a
        det=b[0]*c[2]-b[2]*c[0]
        if abs(det)<1e-7: continue
        u=((x-a[0])*c[2]-(z-a[2])*c[0])/det
        t=(b[0]*(z-a[2])-b[2]*(x-a[0]))/det
        y=a[1]+b[1]*u+c[1]*t
        valid=(u>=-.001)&(t>=-.001)&(u+t<=1.001)&(y>=-5.5)&(y<=-2)
        best=floor[sl]
        np.copyto(best,y,where=valid & (np.isnan(best)|(abs(y+4)<abs(best+4))))
    blocked=np.zeros_like(floor,dtype=bool)
    for v in triangles[abs(faces[:,9])<.62]:
        if v[:,1].min()>0 or v[:,1].max()<-5.25: continue
        x,z,sl=area(v)
        mask=(v[:,1].min()<floor[sl]+1.65)&(v[:,1].max()>floor[sl]+.32)
        for a,b in zip(v,np.roll(v,-1,axis=0)):
            dx,dz=(b-a)[[0,2]]
            t=np.clip(((x-a[0])*dx+(z-a[2])*dz)/max(1e-7,dx*dx+dz*dz),0,1)
            blocked[sl]|=mask & ((x-a[0]-t*dx)**2+(z-a[2]-t*dz)**2<.42**2)
    dest.write_bytes(struct.pack('<4h',*lo,w,h)+(np.isfinite(floor)&~blocked).astype('u1').tobytes())
