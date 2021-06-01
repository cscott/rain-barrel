function inch() = 25.4;
function hole_clear() = 0.25;
function magtag_width_spacing() = 77.9 - 5.5;
function magtag_height_spacing() = 51.2 - 5.5;

$fn=28;

module cubexyz(dims, cx=false, cy=false, cz=false) {
    translate([cx?-dims.x/2:0, cy?-dims.y/2:0, cz?-dims.z/2:0])
        cube(dims);
}

module main(holes=true) {
    thickness = 3;
    total_height = 26.8-9;

    bottom_spacing = 80.3; // from the diagram
    bottom_width = 12;
    bottom_hole_diam = 4.5 + hole_clear(); // m4 clear

    //magtag_width_spacing = 77.9 - 5.5;
    //magtag_height_spacing = 51.2 - 5.5;
    magtag_hole_diam = 2.5 + hole_clear(); // m3 tap
    magtag_support_diam = 8;

    for (i=[1,-1]) scale([i,1,1]) translate([bottom_spacing/2,0,0]) {
        if (holes) {
            translate([0,0,-1])
            cylinder(d=bottom_hole_diam, h=thickness+4);
        } else {
            cylinder(d=bottom_hole_diam+4, h=thickness+1);
        }
    }
    if (!holes) {
        cubexyz([bottom_spacing+bottom_width, bottom_width, thickness],
                cx=true, cy=true);
    }
    for (i=[1,-1]) scale([i,1,1]) translate([magtag_width_spacing()/2,0,0]) {
            if (!holes) {
                cubexyz([bottom_width, magtag_height_spacing() + bottom_width, thickness], cx=true, cy=true);
            }
            for (j=[1,-1]) scale([1,j,1]) translate([0,magtag_height_spacing()/2,0]) {
                    if (holes) {
                        translate([0,0,-1]) cylinder(d=magtag_hole_diam, h=total_height+2);
                    } else {
                        cylinder(d=magtag_support_diam, h=total_height);
                    }
            }
    }
}

intersection() {
    difference() {
        main(holes=false);
        main(holes=true);
    }
    minkowski() {
        union() {
        cubexyz([magtag_width_spacing(), magtag_height_spacing(), 30],
                cx=true, cy=true);
        cubexyz([200,12-5.5,30],cx=true,cy=true);
        }
        cylinder(d=5.5, h=1, center=true);
    }
}
