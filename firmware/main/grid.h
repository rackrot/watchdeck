#pragma once

/*
 * The button grid's geometry, with no LVGL so that a host test can pin it. The numbers are mirrored in
 * app/src-tauri/src/daemon/icons.rs (box_px, which sizes the icons the daemon sends) and app/src/preview.ts
 * (the editor's preview). Change all three together.
 */

#include <stdbool.h>

#define GRID_SCREEN_W 1024 // checked against DISPLAY_H_RES in ui.c
#define GRID_SCREEN_H 600  // checked against DISPLAY_V_RES in ui.c
#define GRID_TOP 204
#define GRID_GAP 8
#define PAGER_H 31 // the "Page x of y" strip under a paged grid: SLOT24_H plus 4 px of margin
#define ICON_PAD 4 // a button with an icon is a column: the icon, a gap, and a label of fixed height
#define ICON_GAP 2
#define ICON_MIN 24       // the label gets two lines unless that leaves the icon less than this
#define TILE_BIG_MIN_H 80 // a tile's value uses the 32 pt font in cells at least this tall, else 24 pt

#define SLOT24_H 27 // Montserrat 24's line height
#define SLOT32_H 35 // Montserrat 32's line height

typedef struct {
    int cell_w;
    int cell_h;
    int label_lines; // lines of label an icon button has room for: 1 or 2
    int icon_side;   // the side of the square an icon fits in
    int grid_h;      // the height the cells divide: the screen below GRID_TOP, less PAGER_H when paged
} grid_geometry_t;

/** The cell size, label lines, icon side and grid height for `cols` x `rows`. `paged` is 2 or more pages. */
grid_geometry_t grid_geometry(int cols, int rows, bool paged);
