"""A concave LDB wall must retain its door opening in all three planes."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from triangulate import triangulate

class PolygonTest(unittest.TestCase):
    def test_door_cut_and_winding(self):
        outline=[(0,0),(4,0),(4,4),(3,4),(3,1),(1,1),(1,4),(0,4)]
        def cross(a,b,c):return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
        for axis in range(3):
            for reverse in (False,True):
                uv=outline[::-1]if reverse else outline
                pts=[tuple(0 if k==axis else p[k-(k>axis)]for k in range(3))for p in uv]
                triangles=triangulate(pts)
                self.assertEqual(len(triangles),6)
                self.assertAlmostEqual(sum(abs(cross(uv[a],uv[b],uv[c]))*.5 for a,b,c in triangles),10)
                for a,b,c in triangles:
                    sign=-1 if cross(uv[a],uv[b],uv[c])<0 else 1
                    self.assertFalse(all(sign*cross(u,v,(2,2))>=-1.e-9 for u,v in [(uv[a],uv[b]),(uv[b],uv[c]),(uv[c],uv[a])]))

if __name__=='__main__':unittest.main()
