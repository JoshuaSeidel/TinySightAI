/*
 * AADongle Camera Unit — Body
 *
 * Pill-shaped (rounded on ALL edges). Holds everything:
 *   - Radxa Cubie A7Z on edge rails (press-fit)
 *   - MFi QFN20 breakout in slide-in slot with clips
 *   - Camera PCB on M2 standoffs, lens extends up through lid
 *
 * Lid sits INTO a stepped rim at the top.
 */

/* ============================================================
 *  DIMENSIONS
 * ============================================================ */

// Radxa Cubie A7Z
radxa_w      = 65;
radxa_d      = 30;
radxa_pcb_h  = 1.2;
radxa_comp_h = 5;

// USB-C OTG power slit (front long edge)
usbc_offset  = 8;       // center from left short edge
usbc_w       = 10;
usbc_h       = 3.8;

// microSD slit (back long edge, bottom of board)
sd_offset    = 32.5;
sd_w         = 13;
sd_h         = 2.5;

// MFi breakout (26 × 13mm)
mfi_w = 26;  mfi_d = 13;  mfi_h = 4.6;

// Camera (Arducam IMX219, 25 × 24mm)
cam_w = 25;  cam_d = 24;
cam_mount_x = 21;   // M2 hole spacing
cam_mount_y = 12.5;
cam_hole_d  = 2.2;
cam_post_od = 4;

// Lens
lens_od   = 14;
lens_above_pcb = 13;

// IR LEDs
ir_d = 5.2;  ir_gap = 16;

// Case
wall = 1.5;  r = 2.5;  tol = 0.3;  fn = 48;

// GoPro
gp_fw = 3;  gp_gap = 3;  gp_hd = 5;  gp_h = 8;

// Vents
vw = 1.5;  vl = 8;  vn = 5;

/* ============================================================
 *  DERIVED
 * ============================================================ */

iw = radxa_w + tol*2;      // inner width
id = radxa_d + tol*2;      // inner depth
sw = iw + wall*2;          // shell width
sd = id + wall*2;          // shell depth

rail = 1.5;                // PCB rail height

// Z positions from case floor (floor = 0)
z_radxa_top = rail + radxa_pcb_h + radxa_comp_h;    // ~7.7
z_cam_pcb   = z_radxa_top + 1.5;                     // 1.5mm clearance above components
z_cam_top   = z_cam_pcb + 1;                          // cam PCB top surface
z_lens_tip  = z_cam_top + lens_above_pcb;             // ~22.2

// Body height: lens pokes through lid, so body goes up to just past cam PCB
// Lid (1.5mm) + bezel (3mm) covers the rest to reach lens tip
lid_thick = 1.5;
bezel_h   = 3;
body_top  = z_lens_tip - lid_thick - bezel_h + 0.5;  // body rim Z (a bit below lens tip)
step      = 3;                                         // lid step depth

body_h = body_top + wall;  // outer height of body shell

/* ============================================================
 *  MODULES
 * ============================================================ */

// Rounded rect: XY corners rounded, flat top/bottom
// Works for ANY height — no minimum constraint
module rrect(w, d, h, cr) {
    hull() {
        for (x = [cr, w-cr])
            for (y = [cr, d-cr])
                translate([x, y, 0])
                    cylinder(r=cr, h=h, $fn=fn);
    }
}

// Pill: fully rounded on ALL edges (XY corners + top/bottom edges)
// Requires w,d,h all > 2*cr
module pill(w, d, h, cr) {
    hull() {
        for (x = [cr, w-cr])
            for (y = [cr, d-cr])
                for (z = [cr, h-cr])
                    translate([x, y, z])
                        sphere(r=cr, $fn=fn);
    }
}

// GoPro 2-prong mount
module gopro() {
    tw = gp_fw*2 + gp_gap;
    translate([sw/2 - tw/2, sd/2, 0]) {
        for (dx = [0, gp_fw + gp_gap]) {
            translate([dx, 0, 0])
            difference() {
                minkowski() {
                    translate([0.3,0.3,0.3])
                        cube([gp_fw-0.6, sd/4-0.6, gp_h-0.6]);
                    sphere(r=0.3, $fn=12);
                }
                translate([gp_fw/2, sd/4+1, gp_h/2])
                    rotate([90,0,0])
                        cylinder(d=gp_hd, h=sd/4+2, $fn=fn);
            }
        }
    }
}

// PCB edge rails
module rails() {
    rd = 2;  // rail depth
    for (y = [wall, sd-wall-rd])
        translate([wall, y, wall])
            cube([iw, rd, rail]);
    for (x = [wall, sw-wall-rd])
        translate([x, wall, wall])
            cube([rd, id, rail]);
}

// MFi slide-in slot with retention clips
module mfi_slot() {
    sz = wall + z_radxa_top;
    rw = 1.0;    // rail wall thickness
    ch = 0.6;    // clip height
    // Centered front-to-back
    by = wall + (id - mfi_d - tol)/2;
    bx = wall + 3;

    translate([bx, by, sz]) {
        // Front rail
        cube([mfi_w + tol + rw, rw, mfi_h]);
        // Back rail
        translate([0, mfi_d + tol, 0])
            cube([mfi_w + tol + rw, rw, mfi_h]);
        // End stop
        translate([mfi_w + tol, 0, 0])
            cube([rw, mfi_d + tol + rw*2, mfi_h]);
        // Retention clips (snap over PCB top)
        for (dy = [rw, mfi_d + tol])
            translate([mfi_w*0.3, dy - ch/2, mfi_h - 0.1])
                cube([5, ch, ch]);
    }
}

// Camera M2 standoffs
module cam_posts() {
    cx = sw/2;  cy = sd/2;
    ph = z_cam_pcb - wall;  // post height from floor

    for (sx = [-1,1])
        for (sy = [-1,1])
            translate([cx + sx*cam_mount_x/2,
                       cy + sy*cam_mount_y/2,
                       wall])
                difference() {
                    cylinder(d=cam_post_od, h=ph, $fn=fn);
                    translate([0,0,-0.1])
                        cylinder(d=cam_hole_d, h=ph+0.2, $fn=fn);
                }
}

// Vent pattern on a face
module vents(h, w) {
    sp = h / (vn+1);
    for (i = [1:vn])
        translate([(w-vl)/2, -0.1, i*sp - vw/2])
            cube([vl, wall + r + 0.5, vw]);
}

/* ============================================================
 *  ASSEMBLY
 * ============================================================ */

module body() {
    difference() {
        union() {
            // Main pill shell
            translate([0, 0, gp_h])
                pill(sw, sd, body_h, r);
            // GoPro
            gopro();
        }

        // Hollow out interior
        translate([wall, wall, gp_h + wall])
            cube([iw, id, body_h + r]);

        // Lid step — widen top rim for lid to sit in
        translate([wall/2, wall/2, gp_h + body_h - step])
            cube([iw + wall, id + wall, step + r + 1]);

        // USB-C power slit (front)
        translate([wall + tol + usbc_offset - usbc_w/2,
                   -r - 0.1,
                   gp_h + wall + rail])
            cube([usbc_w, wall + r + 1, usbc_h]);

        // MicroSD slit (back)
        translate([wall + tol + sd_offset - sd_w/2,
                   sd - wall - r - 0.1,
                   gp_h + wall - 0.1])
            cube([sd_w, wall + r + 1, sd_h + 0.2]);

        // MFi board insertion slot (left short edge)
        mfi_by = wall + (id - mfi_d - tol)/2;
        translate([-r - 0.1, mfi_by - 0.5,
                   gp_h + wall + z_radxa_top])
            cube([wall + r + 1, mfi_d + tol + 2, mfi_h * 0.7]);

        // Vents — both long sides
        translate([0, 0, gp_h])
            vents(body_h, sw);
        translate([0, sd - wall, gp_h])
            vents(body_h, sw);
    }

    // Internal features
    translate([0, 0, gp_h]) {
        rails();
        mfi_slot();
        cam_posts();
    }
}

body();
