use <standoff.scad>;
$fn=28;

module lid() {
    translate([-31,-11.5,-12])
    import("feather-top.stl", convexity=16);
}
module magtag_support(holes=false) {
    thickness = 2;
    extra=8;
    extra_height=15;
    wire_height_space=6;
    wire_diam = 9; // big enough to fit the connector through
    wire_width_space=16.5;

    if (!holes) {
        translate([0,-extra_height/2,0]) {
            minkowski() {
                cubexyz([magtag_width_spacing(),
                         magtag_height_spacing()+extra_height,
                         thickness-1], cx=true, cy=true);
                cylinder(d=extra,h=1);
            }
        }
    } else {
        for (i=[-1,1]) for (j=[-1,1]) scale([i,j,1]) {
            translate([magtag_width_spacing()/2, magtag_height_spacing()/2, -1])
                cylinder(d=3, h=thickness+2);
        }
        for (i=[0,1]) {
            translate([i*wire_width_space, -magtag_height_spacing()/2-wire_height_space, -1]) {
                cylinder(d=wire_diam, h=thickness+2);
                if (i==0) cubexyz([wire_width_space, wire_diam, thickness+2], cy=true);
            }
        }
    }
}
module newtop(holes=false) {
    if (!holes) { lid(); }
    translate([0,17,2])
    magtag_support(holes=holes);
}
difference() {
newtop(holes=false);
newtop(holes=true);
}
