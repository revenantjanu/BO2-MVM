// Host-side test for camera.h. Build: g++ -std=c++17 -I../dll/src test_camera.cpp -o test_camera
#include "camera.h"
#include <cassert>
#include <iostream>
using namespace mvm;
static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }
int main() {
    Vec3 f, r, u;
    AngleVectors(0, 0, 0, &f, &r, &u);
    assert(near(f.x, 1) && near(f.y, 0) && near(f.z, 0));
    assert(near(r.x, 0) && near(r.y, -1));     // engine: +Y is left, so right is -Y
    assert(near(u.z, 1));
    AngleVectors(0, 90, 0, &f, &r, &u);
    assert(near(f.y, 1) && near(r.x, 1));
    AngleVectors(30, 45, 0, &f, nullptr, nullptr);
    float p, y; ForwardToAngles(f, &p, &y);
    assert(near(p, 30) && near(y, 45));
    // angle wrap
    assert(near(LerpAngle(350, 10, 0.5f), 360.0f) || near(LerpAngle(350, 10, 0.5f), 0.0f));
    // path
    CamPath path; path.interp = Interp::Linear;
    CamKey a, b; a.timeMs = 0; b.timeMs = 1000; b.pose.pos.x = 100; b.pose.yaw = 90; b.pose.fov = 90; a.pose.fov = 60;
    path.keys = {b, a}; path.Sort();
    CamPose o; assert(path.Sample(500, &o));
    assert(near(o.pos.x, 50) && near(o.yaw, 45) && near(o.fov, 75));
    assert(path.Sample(-10, &o) && near(o.pos.x, 0));
    assert(path.Sample(5000, &o) && near(o.pos.x, 100));
    // smooth path passes through keys
    CamPath s; s.interp = Interp::Smooth;
    for (int i = 0; i < 4; ++i) { CamKey k; k.timeMs = i * 500.0; k.pose.pos.x = (float)(i * i * 10); s.keys.push_back(k); }
    for (int i = 0; i < 4; ++i) { s.Sample(i * 500.0, &o); assert(near(o.pos.x, (float)(i * i * 10), 0.01f)); }
    // yaw wrap through the short way with smooth interp
    CamPath w; w.interp = Interp::Smooth;
    for (int i = 0; i < 3; ++i) { CamKey k; k.timeMs = i * 1000.0; k.pose.yaw = NormAngle180(350.0f + i * 10.0f); w.keys.push_back(k); }
    w.Sample(500, &o); assert(std::fabs(NormAngle180(o.yaw - 355)) < 1.0f);
    // save / load round trip 
    path.Save("/tmp/t.bo2path"); CamPath l; assert(l.Load("/tmp/t.bo2path")); assert(l.keys.size() == 2 && near(l.keys[1].pose.pos.x, 100));
    FILE* fp = fopen("/tmp/t.bo2path", "w");
    fputs("bo2campath 1\ninterp 0\ncam 1707905.590 -867.4161 780.7630 1039.5321 -10.5473 -193.9174 -43.0746 51.4076\n", fp); fclose(fp);
    CamPath c; assert(c.Load("/tmp/t.bo2path") && c.keys.size() == 1 && near(c.keys[0].pose.fov, 51.4076f, 1e-3f));
    // freecam moves along look direction
    FreeCam fc; FreeCamSettings fs; fs.smoothMove = 0; fs.smoothLook = 0; FreeCamInput in; in.forward = 1;
    fc.Reset(CamPose()); fc.Step(1.0f, in, fs, 0, 0); assert(near(fc.pose.pos.x, 260));
    std::cout << "camera tests OK\n";
}
