"""Triangulate ordered LDB polygons without filling concave door/window cuts."""
def triangulate(points):
    points=list(points)
    if len(points)<3:return []
    normal=[0.,0.,0.]
    for a,b in zip(points,points[1:]+points[:1]):
        normal[0]+=(a[1]-b[1])*(a[2]+b[2])
        normal[1]+=(a[2]-b[2])*(a[0]+b[0])
        normal[2]+=(a[0]-b[0])*(a[1]+b[1])
    axis=max(range(3),key=lambda k:abs(normal[k]))
    axes=[k for k in range(3) if k!=axis]
    uv=[(float(p[axes[0]]),float(p[axes[1]]))for p in points]
    def area(a,b,c):return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    span=max(max(p[k]for p in uv)-min(p[k]for p in uv)for k in range(2))
    eps=max(1.e-12,span*span*1.e-10)
    def same(a,b):return (a[0]-b[0])**2+(a[1]-b[1])**2<=eps*1.e-4
    ring=[]
    for i in range(len(uv)):
        if not ring or not same(uv[ring[-1]],uv[i]):ring.append(i)
    if len(ring)>1 and same(uv[ring[0]],uv[ring[-1]]):ring.pop()
    total=sum(area(uv[ring[0]],uv[ring[i]],uv[ring[i+1]])for i in range(1,len(ring)-1)) if ring else 0
    if abs(total)<=eps:return []
    sign=1 if total>0 else -1
    # Most BSP faces are convex. Preserve their previous triangle/index order.
    if all(sign*area(uv[ring[i-1]],uv[ring[i]],uv[ring[(i+1)%len(ring)]])>=-eps for i in range(len(ring))):
        return [(ring[0],ring[i],ring[i+1])for i in range(1,len(ring)-1)
                if abs(area(uv[ring[0]],uv[ring[i]],uv[ring[i+1]]))>eps]
    result=[]
    while len(ring)>2:
        found=False
        for j,b in enumerate(ring):
            a,c=ring[j-1],ring[(j+1)%len(ring)]
            turn=sign*area(uv[a],uv[b],uv[c])
            if turn<-eps:continue
            if turn>eps:
                occupied=False
                for k in ring:
                    if k in (a,b,c) or any(same(uv[k],uv[v])for v in (a,b,c)):continue
                    if min(sign*area(uv[a],uv[b],uv[k]),sign*area(uv[b],uv[c],uv[k]),sign*area(uv[c],uv[a],uv[k]))>=-eps:
                        occupied=True;break
                if occupied:continue
                result.append((a,b,c))
            ring.pop(j);found=True;break
        if not found:raise ValueError('LDB polygon is not a simple planar ring')
    return result
