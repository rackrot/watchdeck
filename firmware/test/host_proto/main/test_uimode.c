// uimode.c: the table of what each UI entry point does in each screen mode.

#include "unity.h"

#include "uimode.h"

// No cell may be left unset: a mode added to ui_mode_t without a column fails here.
void test_uimode_every_cell_is_decided(void)
{
    for (int e = 0; e < UIM_ENTRY_COUNT; e++) {
        for (int m = 0; m < MODE_COUNT; m++) {
            char msg[48];
            snprintf(msg, sizeof(msg), "entry %d mode %d", e, m);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(UI_ACTION_NONE, uimode_action(e, m), msg);
        }
    }
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, uimode_action(UIM_ENTRY_COUNT, MODE_GRID));
    TEST_ASSERT_EQUAL(UI_ACTION_NONE, uimode_action(UIM_SHOW_SET, MODE_COUNT));
}

// Asleep, nothing builds a widget: a set waits for the wake, a tile or icon stays in the model, a result is only
// logged, a pairing view has no place.
void test_uimode_asleep_builds_nothing(void)
{
    for (int e = 0; e < UIM_ENTRY_COUNT; e++) {
        TEST_ASSERT_NOT_EQUAL(UI_BUILD, uimode_action(e, MODE_SLEEP));
    }
    TEST_ASSERT_EQUAL(UI_DEFER, uimode_action(UIM_SHOW_SET, MODE_SLEEP));
    TEST_ASSERT_EQUAL(UI_DROP, uimode_action(UIM_SET_TILE, MODE_SLEEP));
    TEST_ASSERT_EQUAL(UI_DROP, uimode_action(UIM_SET_ICON, MODE_SLEEP));
    TEST_ASSERT_EQUAL(UI_DROP, uimode_action(UIM_SHOW_RESULT, MODE_SLEEP));
    TEST_ASSERT_EQUAL(UI_DROP, uimode_action(UIM_PAIR_CODE, MODE_SLEEP));
}

// The modes that existed before sleep behave as they did: settings defers a set and shows a result over itself.
void test_uimode_settings_and_grid_keep_their_behavior(void)
{
    TEST_ASSERT_EQUAL(UI_DEFER, uimode_action(UIM_SHOW_SET, MODE_SETTINGS));
    TEST_ASSERT_EQUAL(UI_DEFER, uimode_action(UIM_DAEMON_LIST, MODE_SETTINGS));
    TEST_ASSERT_EQUAL(UI_BUILD, uimode_action(UIM_SHOW_RESULT, MODE_SETTINGS));
    TEST_ASSERT_EQUAL(UI_BUILD, uimode_action(UIM_PAIR_CODE, MODE_SETTINGS));
    TEST_ASSERT_EQUAL(UI_BUILD, uimode_action(UIM_SET_TILE, MODE_GRID));
    TEST_ASSERT_EQUAL(UI_BUILD, uimode_action(UIM_SET_ICON, MODE_GRID));
    TEST_ASSERT_EQUAL(UI_BUILD, uimode_action(UIM_SHOW_SET, MODE_STATUS));
    TEST_ASSERT_EQUAL(UI_BUILD, uimode_action(UIM_DAEMON_LIST, MODE_STATUS));
}

void test_uimode_only_an_offline_sleep_wakes_on_the_link(void)
{
    TEST_ASSERT_TRUE(uimode_link_wakes(SLEEP_OFFLINE));
    TEST_ASSERT_FALSE(uimode_link_wakes(SLEEP_PRIVACY));
}

void test_uimode_can_sleep(void)
{
    // Unpaired never sleeps, in any mode, for either reason.
    for (int m = 0; m < MODE_COUNT; m++) {
        TEST_ASSERT_FALSE(uimode_can_sleep(m, SLEEP_PRIVACY, false));
        TEST_ASSERT_FALSE(uimode_can_sleep(m, SLEEP_OFFLINE, false));
    }
    TEST_ASSERT_TRUE(uimode_can_sleep(MODE_GRID, SLEEP_PRIVACY, true));
    TEST_ASSERT_TRUE(uimode_can_sleep(MODE_GRID, SLEEP_OFFLINE, true));
    TEST_ASSERT_TRUE(uimode_can_sleep(MODE_STATUS, SLEEP_OFFLINE, true));
    TEST_ASSERT_FALSE(uimode_can_sleep(MODE_STATUS, SLEEP_PRIVACY, true));
    // Never from settings, never when already asleep.
    TEST_ASSERT_FALSE(uimode_can_sleep(MODE_SETTINGS, SLEEP_PRIVACY, true));
    TEST_ASSERT_FALSE(uimode_can_sleep(MODE_SETTINGS, SLEEP_OFFLINE, true));
    TEST_ASSERT_FALSE(uimode_can_sleep(MODE_SLEEP, SLEEP_PRIVACY, true));
    TEST_ASSERT_FALSE(uimode_can_sleep(MODE_SLEEP, SLEEP_OFFLINE, true));
}

void test_uimode_run(void)
{
    RUN_TEST(test_uimode_can_sleep);
    RUN_TEST(test_uimode_every_cell_is_decided);
    RUN_TEST(test_uimode_asleep_builds_nothing);
    RUN_TEST(test_uimode_settings_and_grid_keep_their_behavior);
    RUN_TEST(test_uimode_only_an_offline_sleep_wakes_on_the_link);
}
