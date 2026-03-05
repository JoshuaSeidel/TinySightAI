/*
 * AADongle Camera Unit — Lid
 *
 * Rounded rectangle cap that sits INTO the body's stepped rim.
 * Small bezel ring around the lens hole — smoothly connected to
 * the lid surface, not floating. IR LED holes flank the bezel.
 *
 * The lens pokes up through the lid from the camera mounted
 * inside the body. The bezel protects the lens edges.
 */

/* ============================================================
 *  DIMENSIONS — must match body
 * ============================================================ */

radxa_w = 65;  radxa_d = 30;
wall = 1.5;  r = 2.5;  tol = 0.3;  fn = 48;

iw = radxa_w + tol*2;
id = radxa_d + tol*2;
sw = iw + wall*2;
sd = id + wall*2;

// Lid geometry
step       = 3;       // how deep lid sits into body
step_tol   = 0.2;     // clearance for fit
step_wall  = 1.2;     // step wall thickness
top_thick  = 2;       // lid plate thickness (solid)

// Lens
lens_od    = 14;
lens_hole  = lens_od + 1;  // 15mm bore

// Bezel — small ring, smoothly joined to lid top
bezel_od   = lens_hole + 4; // 19mm — just enough to protect lens
bezel_h    = 3;              // 3mm rise above lid surface
bezel_fillet = 1;            // smooth transition at base

// IR LEDs
ir_d = 5.2;  ir_gap = 16;

// CSI ribbon slot
csi_w = 18;  csi_h = 1.5;

// Vents
vw = 1.5;  vl = 8;  vn = 3;

/* ============================================================
 *  MODULES
 * ============================================================ */

// Rounded rectangle — XY rounded, flat top/bottom
module rrect(w, d, h, cr) {
    hull() {
        for (x = [cr, w-cr])
            for (y = [cr, d-cr])
                translate([x, y, 0])
                    cylinder(r=cr, h=h, $fn=fn);
    }
}

// Bezel ring with fillet at base (smooth transition to lid surface)
module bezel(od, id, h, fillet) {
    union() {
        // Main ring
        difference() {
            cylinder(d=od, h=h, $fn=fn);
            translate([0, 0, -0.1])
                cylinder(d=id, h=h + 0.2, $fn=fn);
        }
        // Fillet at base — torus section for smooth transition
        difference() {
            rotate_extrude($fn=fn)
                translate([od/2 - fillet, 0, 0])
                    circle(r=fillet, $fn=fn/2);
            // Cut below Z=0
            translate([-(od+4)/2, -(od+4)/2, -od])
                cube([od+4, od+4, od]);
            // Cut inner bore
            translate([0, 0, -0.1])
                cylinder(d=id, h=fillet + 0.2, $fn=fn);
        }
    }
}

// Vent slots
module lid_vents(h, w) {
    sp = h / (vn+1);
    for (i = [1:vn])
        translate([(w-vl)/2, -0.1, i*sp - vw/2])
            cube([vl, step_wall + 0.2, vw]);
}

/* ============================================================
 *  ASSEMBLY
 * ============================================================ */

module lid() {
    cx = sw/2;
    cy = sd/2;

    // Step dimensions (part that drops into body)
    stw = iw + wall - step_tol*2;    // step outer width
    std = id + wall - step_tol*2;    // step outer depth

    difference() {
        union() {
            // === TOP PLATE === (rounded rectangle, matches body profile)
            rrect(sw, sd, top_thick, r);

            // === BEZEL RING === (on top of plate, centered)
            translate([cx, cy, top_thick - 0.1])
                bezel(bezel_od, lens_hole, bezel_h, bezel_fillet);

            // === STEP WALLS === (drop into body)
            // Outer step shell (rounded rect, hanging below)
            translate([(sw - stw)/2, (sd - std)/2, -step])
                rrect(stw, std, step + 0.1, r - wall/2);
        }

        // Hollow out step interior (leave only walls)
        translate([(sw - stw)/2 + step_wall,
                   (sd - std)/2 + step_wall,
                   -step - 0.1])
            cube([stw - step_wall*2,
                  std - step_wall*2,
                  step + 0.2]);

        // === LENS HOLE === (through everything)
        translate([cx, cy, -step - 0.1])
            cylinder(d=lens_hole, h=top_thick + step + bezel_h + bezel_fillet + 1,
                     $fn=fn);

        // === IR LED HOLES === (through lid plate)
        for (sy = [-1, 1])
            translate([cx, cy + sy*ir_gap, -step - 0.1])
                cylinder(d=ir_d, h=top_thick + step + 1, $fn=fn);

        // === CSI RIBBON SLOT === (back step wall)
        translate([cx - csi_w/2,
                   (sd + std)/2 - step_wall - 0.1,
                   -step - 0.1])
            cube([csi_w, step_wall + 0.2, csi_h + 0.1]);

        // === VENT SLOTS === (on step walls, both long sides)
        translate([(sw-stw)/2, (sd-std)/2, -step])
            lid_vents(step, stw);
        translate([(sw-stw)/2, (sd+std)/2 - step_wall, -step])
            lid_vents(step, stw);
    }
}

lid();
