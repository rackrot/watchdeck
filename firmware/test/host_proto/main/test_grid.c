// Host tests for grid.c: the Geometry table of plans/pages.md, row for row. icons.rs
// `the_box_follows_the_firmware_button` and app/tests/preview.test.ts pin the same table on the daemon and preview
// sides.
#include <stdio.h>

#include "grid.h"
#include "proto.h"
#include "unity.h"

// Rows 1 to 5: { cell_h, label_lines, icon_side } on the full grid (396 px), then on the paged grid (365 px).
// The width cap (cols = 1) is 1000 px, so no row here is bound by width.
static const struct {
    int cell_h, label_lines, icon_side;
} FULL[5] = {{388, 2, 256}, {190, 2, 110}, {124, 2, 44}, {91, 1, 46}, {71, 1, 26}};
static const struct {
    int cell_h, label_lines, icon_side;
} PAGED[5] = {{357, 2, 256}, {174, 2, 94}, {113, 2, 33}, {83, 1, 38}, {65, 1, 20}};

// cell width less 2 * ICON_PAD, for 1 to 8 columns
static const int WIDTH_CAP[8] = {1000, 492, 322, 238, 187, 153, 129, 111};

void test_grid_table_matches_the_plan_row_for_row(void)
{
    for (int rows = 1; rows <= 5; rows++) {
        const grid_geometry_t f = grid_geometry(1, rows, false);
        TEST_ASSERT_EQUAL_INT_MESSAGE(396, f.grid_h, "full grid height");
        TEST_ASSERT_EQUAL_INT_MESSAGE(FULL[rows - 1].cell_h, f.cell_h, "full cell_h");
        TEST_ASSERT_EQUAL_INT_MESSAGE(FULL[rows - 1].label_lines, f.label_lines, "full label_lines");
        TEST_ASSERT_EQUAL_INT_MESSAGE(FULL[rows - 1].icon_side, f.icon_side, "full icon_side");

        const grid_geometry_t p = grid_geometry(1, rows, true);
        TEST_ASSERT_EQUAL_INT_MESSAGE(365, p.grid_h, "paged grid height");
        TEST_ASSERT_EQUAL_INT_MESSAGE(PAGED[rows - 1].cell_h, p.cell_h, "paged cell_h");
        TEST_ASSERT_EQUAL_INT_MESSAGE(PAGED[rows - 1].label_lines, p.label_lines, "paged label_lines");
        TEST_ASSERT_EQUAL_INT_MESSAGE(PAGED[rows - 1].icon_side, p.icon_side, "paged icon_side");
    }
}

void test_grid_width_cap_for_1_to_8_columns(void)
{
    for (int cols = 1; cols <= 8; cols++) {
        const grid_geometry_t f = grid_geometry(cols, 1, false);
        TEST_ASSERT_EQUAL_INT_MESSAGE(WIDTH_CAP[cols - 1], f.cell_w - 2 * ICON_PAD, "cell width less padding");
    }
    // One row of 4 or more columns is bound by width, paged or not (238 px at 4 columns).
    TEST_ASSERT_EQUAL_INT(238, grid_geometry(4, 1, false).icon_side);
    TEST_ASSERT_EQUAL_INT(238, grid_geometry(4, 1, true).icon_side);
    TEST_ASSERT_EQUAL_INT(111, grid_geometry(8, 1, true).icon_side);
}

void test_grid_pager_is_a_slot_24_line_plus_4_px_of_margin(void)
{
    TEST_ASSERT_EQUAL_INT(SLOT24_H + 4, PAGER_H);
}

// The slots fit every cell: a tile without an icon, a tile with an icon and an icon button, as build_grid lays them
// out.
void test_grid_slots_fit_every_cell_of_every_grid(void)
{
    for (int paged = 0; paged <= 1; paged++) {
        for (int cols = 1; cols <= 8; cols++) {
            for (int rows = 1; rows <= 5; rows++) {
                const grid_geometry_t g = grid_geometry(cols, rows, paged);
                const int value_h = g.cell_h >= TILE_BIG_MIN_H ? SLOT32_H : SLOT24_H;
                // a label of 24 pt, a gap, the value
                const int tile = SLOT24_H + ICON_GAP + value_h + 2 * ICON_PAD;
                // the icon and the value; the 24 pt label too when there are two label lines
                const int tile_icon =
                    g.icon_side + ICON_GAP + value_h + 2 * ICON_PAD + (g.label_lines == 2 ? SLOT24_H + ICON_GAP : 0);
                const int icon_button = g.icon_side + ICON_GAP + g.label_lines * SLOT32_H + 2 * ICON_PAD;
                char msg[64];
                snprintf(msg, sizeof(msg), "%dx%d paged %d", cols, rows, paged);
                TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(g.cell_h, tile, msg);
                TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(g.cell_h, tile_icon, msg);
                TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(g.cell_h, icon_button, msg);
            }
        }
    }
    // The exact fits named in the plan.
    TEST_ASSERT_EQUAL_INT(64, SLOT24_H + ICON_GAP + SLOT24_H + 2 * ICON_PAD); // 5 rows paged, no icon, of 65
    TEST_ASSERT_EQUAL_INT(83, 38 + ICON_GAP + SLOT32_H + 2 * ICON_PAD);       // 4 rows paged, tile with icon, of 83
}

// The board caches the icons of every page of a set (icons.h: ICONS_MAX entries, ICONS_MAX_BYTES of 8 MiB). A valid
// set names at most one icon per cell per page, each at most icon_side square, so it never reaches either cap. icons.rs
// pins the same bound on the daemon side.
void test_grid_icon_cache_bound_holds_for_every_set(void)
{
    TEST_ASSERT_EQUAL_INT(160, PROTO_MAX_PAGES * PROTO_MAX_BUTTONS);
    long worst = 0;
    for (int cols = 1; cols <= PROTO_MAX_COLS; cols++) {
        for (int rows = 1; rows <= PROTO_MAX_ROWS; rows++) {
            const long cells = cols * rows;
            const long paged_side = grid_geometry(cols, rows, true).icon_side;
            const long full_side = grid_geometry(cols, rows, false).icon_side;
            const long paged = PROTO_MAX_PAGES * cells * paged_side * paged_side * 4;
            const long full = cells * full_side * full_side * 4;
            worst = paged > worst ? paged : worst;
            worst = full > worst ? full : worst;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT64(8 * 1024 * 1024, worst);
    TEST_ASSERT_EQUAL_INT64(3625216, worst); // 4 pages of 4x1, 16 icons of 238 x 238
}

void test_grid_run(void)
{
    RUN_TEST(test_grid_table_matches_the_plan_row_for_row);
    RUN_TEST(test_grid_width_cap_for_1_to_8_columns);
    RUN_TEST(test_grid_pager_is_a_slot_24_line_plus_4_px_of_margin);
    RUN_TEST(test_grid_slots_fit_every_cell_of_every_grid);
    RUN_TEST(test_grid_icon_cache_bound_holds_for_every_set);
}
