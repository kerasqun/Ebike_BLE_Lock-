// 电动车 BLE 解锁模块外壳 v0.1  单位 mm
// 内腔 80x50x25，壁厚 2，PETG 打印，0.2 层高，20% 填充
$fn=48;
L=80; W=50; H=25; T=2; LID=3;
module body(){
  difference(){
    union(){
      cube([L+2*T,W+2*T,H+T]);
      // 车架扎带耳，两侧各一个，扎带宽 5mm 以内
      for(y=[ -6, W+2*T]) translate([L/2+T-10,y,0]) difference(){
        cube([20,6,4]); translate([7.5,1.5,-1]) cube([5,3,6]);
      }
    }
    translate([T,T,T]) cube([L,W,H+1]);
    // 格兰头孔 PG7 (12.5mm)，在 -X 端
    translate([-1,T+W/2,T+H/2]) rotate([0,90,0]) cylinder(d=12.5,h=T+2);
    // 蜂鸣器出声孔 2mm，+Y 侧
    translate([T+60,W+T-1,T+12]) rotate([-90,0,0]) cylinder(d=2,h=T+2);
  }
  // C3 板卡槽（18x23.5），放 +X 端，天线朝外壁
  translate([L+T-25,T+W/2-10.5,T]) difference(){ cube([25,21,4]); translate([1,1,1.5]) cube([23.5,19,3]); }
  // SW-420 的 M3 铜柱底座（孔距按模块实物再核，默认 32x14 中的一颗孔）
  translate([T+15,T+10,T]) difference(){ cylinder(d=6,h=5); cylinder(d=2.6,h=6); }
  // 降压模块和继电器用双面胶或热熔胶固定在盒底中段
  // 盖子 4 角 M3 自攻螺丝柱
  for(x=[T+3,L+T-3]) for(y=[T+3,W+T-3]) translate([x,y,T]) difference(){ cylinder(d=6,h=H); cylinder(d=2.5,h=H+1); }
}
module lid(){
  difference(){
    union(){ cube([L+2*T,W+2*T,LID]); translate([T+0.3,T+0.3,LID]) difference(){ cube([L-0.6,W-0.6,2]); translate([1.2,1.2,-1]) cube([L-3,W-3,4]); } }
    for(x=[T+3,L+T-3]) for(y=[T+3,W+T-3]) translate([x,y,-1]) cylinder(d=3.4,h=LID+4);
  }
}
part="body"; // "body" 或 "lid"
if(part=="body") body(); else lid();
