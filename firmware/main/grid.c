#include "grid.h"

#include "proto.h"

grid_geometry_t grid_geometry(int cols, int rows, bool paged)
{
    grid_geometry_t g;
    g.grid_h = GRID_SCREEN_H - GRID_TOP - (paged ? PAGER_H : 0);
    g.cell_w = (GRID_SCREEN_W - (cols + 1) * GRID_GAP) / cols;
    g.cell_h = (g.grid_h - rows * GRID_GAP) / rows;
    // Slot line boxes, never a font's own line height, so that no font moves the geometry.
    const int room = g.cell_h - 2 * ICON_PAD;
    g.label_lines = room >= 2 * SLOT32_H + ICON_GAP + ICON_MIN ? 2 : 1;
    g.icon_side = room - g.label_lines * SLOT32_H - ICON_GAP;
    if (g.icon_side > g.cell_w - 2 * ICON_PAD) {
        g.icon_side = g.cell_w - 2 * ICON_PAD;
    }
    if (g.icon_side > PROTO_ICON_MAX_SIDE) {
        g.icon_side = PROTO_ICON_MAX_SIDE;
    }
    return g;
}
