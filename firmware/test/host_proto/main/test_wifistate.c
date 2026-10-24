// Host tests for wifistate.c: the connect machine's transitions and the scan list's merge.
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "wifistate.h"

#define R_ASSOC_LEAVE 8
#define R_4WAY_TIMEOUT 15
#define R_NO_AP 201
#define R_AUTH_FAIL 202
#define R_CONN_FAIL 205

static wifi_machine_t m;

// Put the machine in `state` by the events that lead there.
static void reach(wifi_conn_state_t state)
{
    wifistate_init(&m);
    switch (state) {
    case WS_IDLE:
        break;
    case WS_CONNECTING:
        wifistate_step(&m, WE_APPLY, 0);
        break;
    case WS_CONNECTED:
        wifistate_step(&m, WE_APPLY, 0);
        wifistate_step(&m, WE_GOT_IP, 1);
        break;
    case WS_BACKOFF:
        wifistate_step(&m, WE_APPLY, 0);
        wifistate_step(&m, WE_DISCONNECTED, R_NO_AP);
        break;
    case WS_TRIAL:
        wifistate_step(&m, WE_TRIAL_BEGIN, 1);
        break;
    case WS_RESTORING:
        wifistate_step(&m, WE_TRIAL_BEGIN, 1);
        wifistate_step(&m, WE_TRIAL_TIMEOUT, 0);
        break;
    }
    TEST_ASSERT_EQUAL(state, m.state);
}

static void assert_has(wifi_step_t out, uint32_t actions)
{
    TEST_ASSERT_EQUAL_HEX32(actions, out.actions & actions);
}

static void assert_lacks(wifi_step_t out, uint32_t actions)
{
    TEST_ASSERT_EQUAL_HEX32(0, out.actions & actions);
}

void test_wifistate_boot_connects_and_a_drop_arms_the_retry(void)
{
    wifistate_init(&m);
    wifi_step_t out = wifistate_step(&m, WE_APPLY, 0);
    TEST_ASSERT_EQUAL_HEX32(WA_SET_CURRENT_CONFIG | WA_CONNECT, out.actions);
    TEST_ASSERT_EQUAL(WS_CONNECTING, m.state);

    out = wifistate_step(&m, WE_GOT_IP, 1);
    TEST_ASSERT_EQUAL(WS_CONNECTED, m.state);

    out = wifistate_step(&m, WE_DISCONNECTED, R_CONN_FAIL);
    TEST_ASSERT_EQUAL_HEX32(WA_ARM_RETRY, out.actions);
    TEST_ASSERT_EQUAL(WS_BACKOFF, m.state);

    out = wifistate_step(&m, WE_RETRY_TIMER, 0);
    TEST_ASSERT_EQUAL_HEX32(WA_CONNECT, out.actions);
    TEST_ASSERT_EQUAL(WS_CONNECTING, m.state);
}

void test_wifistate_a_retry_tick_outside_backoff_does_nothing(void)
{
    const wifi_conn_state_t others[] = {WS_IDLE, WS_CONNECTING, WS_CONNECTED, WS_TRIAL, WS_RESTORING};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        reach(others[i]);
        const wifi_step_t out = wifistate_step(&m, WE_RETRY_TIMER, 0);
        TEST_ASSERT_EQUAL_HEX32(0, out.actions);
        TEST_ASSERT_EQUAL(others[i], m.state);
    }
}

void test_wifistate_a_retry_tick_during_a_scan_waits_another_period(void)
{
    reach(WS_BACKOFF);
    const wifi_step_t out = wifistate_step(&m, WE_RETRY_TIMER, 1);
    TEST_ASSERT_EQUAL_HEX32(WA_ARM_RETRY, out.actions);
    TEST_ASSERT_EQUAL(WS_BACKOFF, m.state);
}

void test_wifistate_a_trial_from_backoff_cancels_the_retry_and_never_arms_it(void)
{
    reach(WS_BACKOFF);
    wifi_step_t out = wifistate_step(&m, WE_TRIAL_BEGIN, 1);
    TEST_ASSERT_EQUAL(WS_TRIAL, m.state);
    assert_has(out, WA_STOP_RETRY | WA_SET_TRIAL_CONFIG | WA_CONNECT | WA_START_TRIAL_TIMER);
    assert_lacks(out, WA_ARM_RETRY | WA_DISCONNECT); // nothing is connected, so nothing to leave

    out = wifistate_step(&m, WE_RETRY_TIMER, 0);
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    out = wifistate_step(&m, WE_DISCONNECTED, R_CONN_FAIL);
    assert_has(out, WA_CONNECT);
    assert_lacks(out, WA_ARM_RETRY | WA_TRIAL_FAILED);
    TEST_ASSERT_EQUAL(WS_TRIAL, m.state);
}

void test_wifistate_a_trial_from_connected_leaves_the_old_network_first(void)
{
    reach(WS_CONNECTED);
    const wifi_step_t out = wifistate_step(&m, WE_TRIAL_BEGIN, 1);
    assert_has(out, WA_STOP_RETRY | WA_DISCONNECT | WA_SET_TRIAL_CONFIG | WA_CONNECT | WA_START_TRIAL_TIMER);
}

void test_wifistate_our_own_disconnect_is_not_a_trial_failure(void)
{
    reach(WS_TRIAL);
    for (int i = 0; i < 5; i++) {
        const wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_ASSOC_LEAVE);
        TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    }
    TEST_ASSERT_EQUAL(WS_TRIAL, m.state);
    TEST_ASSERT_EQUAL(0, m.wrong_pass);
    TEST_ASSERT_EQUAL(0, m.not_found);
}

void test_wifistate_a_got_ip_for_another_ssid_does_not_end_the_trial(void)
{
    reach(WS_TRIAL);
    wifi_step_t out = wifistate_step(&m, WE_GOT_IP, 0);
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    TEST_ASSERT_EQUAL(WS_TRIAL, m.state);

    out = wifistate_step(&m, WE_GOT_IP, 1);
    TEST_ASSERT_EQUAL_HEX32(WA_STOP_TRIAL_TIMER | WA_TRIAL_OK, out.actions);
    TEST_ASSERT_EQUAL(WS_CONNECTED, m.state);
}

void test_wifistate_two_wrong_password_disconnects_end_the_trial_and_restore(void)
{
    reach(WS_TRIAL);
    wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_4WAY_TIMEOUT);
    assert_has(out, WA_CONNECT);
    assert_lacks(out, WA_TRIAL_FAILED);

    out = wifistate_step(&m, WE_DISCONNECTED, R_AUTH_FAIL); // either reason counts
    assert_has(out, WA_STOP_TRIAL_TIMER | WA_DISCONNECT | WA_SET_CURRENT_CONFIG | WA_CONNECT | WA_TRIAL_FAILED);
    TEST_ASSERT_EQUAL(WF_WRONG_PASSWORD, out.fail);
    TEST_ASSERT_EQUAL(WS_RESTORING, m.state);
}

void test_wifistate_one_wrong_password_and_another_failure_keep_trying(void)
{
    reach(WS_TRIAL);
    wifistate_step(&m, WE_DISCONNECTED, R_4WAY_TIMEOUT);
    const wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_CONN_FAIL);
    assert_has(out, WA_CONNECT);
    assert_lacks(out, WA_TRIAL_FAILED);
    TEST_ASSERT_EQUAL(WS_TRIAL, m.state);
}

void test_wifistate_two_network_not_found_end_the_trial(void)
{
    reach(WS_TRIAL);
    wifistate_step(&m, WE_DISCONNECTED, R_NO_AP);
    const wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_NO_AP);
    assert_has(out, WA_TRIAL_FAILED);
    TEST_ASSERT_EQUAL(WF_NOT_FOUND, out.fail);
    TEST_ASSERT_EQUAL(WS_RESTORING, m.state);
}

void test_wifistate_the_20_s_timeout_restores(void)
{
    reach(WS_TRIAL);
    const wifi_step_t out = wifistate_step(&m, WE_TRIAL_TIMEOUT, 0);
    assert_has(out, WA_STOP_TRIAL_TIMER | WA_DISCONNECT | WA_SET_CURRENT_CONFIG | WA_CONNECT | WA_TRIAL_FAILED);
    TEST_ASSERT_EQUAL(WF_TIMEOUT, out.fail);
    TEST_ASSERT_EQUAL(WS_RESTORING, m.state);
}

void test_wifistate_a_timeout_after_one_wrong_password_names_the_password(void)
{
    reach(WS_TRIAL);
    wifistate_step(&m, WE_DISCONNECTED, R_AUTH_FAIL);
    const wifi_step_t out = wifistate_step(&m, WE_TRIAL_TIMEOUT, 0);
    TEST_ASSERT_EQUAL(WF_WRONG_PASSWORD, out.fail);
}

void test_wifistate_a_stale_timeout_outside_a_trial_does_nothing(void)
{
    reach(WS_CONNECTED);
    const wifi_step_t out = wifistate_step(&m, WE_TRIAL_TIMEOUT, 0);
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    TEST_ASSERT_EQUAL(WS_CONNECTED, m.state);
}

void test_wifistate_a_restore_failure_falls_to_backoff_with_the_old_credentials(void)
{
    reach(WS_RESTORING);
    wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_ASSOC_LEAVE); // our own disconnect ahead of the restore
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    TEST_ASSERT_EQUAL(WS_RESTORING, m.state);

    out = wifistate_step(&m, WE_GOT_IP, 0); // a straggler of the failed network
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    TEST_ASSERT_EQUAL(WS_RESTORING, m.state);

    out = wifistate_step(&m, WE_DISCONNECTED, R_NO_AP);
    TEST_ASSERT_EQUAL_HEX32(WA_ARM_RETRY, out.actions);
    TEST_ASSERT_EQUAL(WS_BACKOFF, m.state);

    // The retry then connects with whatever config is set, which the restore made the old one.
    out = wifistate_step(&m, WE_RETRY_TIMER, 0);
    TEST_ASSERT_EQUAL_HEX32(WA_CONNECT, out.actions);
}

void test_wifistate_the_old_network_coming_back_ends_the_restore(void)
{
    reach(WS_RESTORING);
    const wifi_step_t out = wifistate_step(&m, WE_GOT_IP, 1);
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    TEST_ASSERT_EQUAL(WS_CONNECTED, m.state);
}

void test_wifistate_a_trial_with_no_old_network_ends_in_idle(void)
{
    wifistate_init(&m);
    wifistate_step(&m, WE_TRIAL_BEGIN, 0);
    wifistate_step(&m, WE_DISCONNECTED, R_AUTH_FAIL);
    const wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_AUTH_FAIL);
    assert_has(out, WA_DISCONNECT | WA_TRIAL_FAILED);
    assert_lacks(out, WA_SET_CURRENT_CONFIG | WA_CONNECT | WA_ARM_RETRY);
    TEST_ASSERT_EQUAL(WS_IDLE, m.state);

    // Stragglers in IDLE change nothing.
    TEST_ASSERT_EQUAL_HEX32(0, wifistate_step(&m, WE_DISCONNECTED, R_NO_AP).actions);
    TEST_ASSERT_EQUAL_HEX32(0, wifistate_step(&m, WE_RETRY_TIMER, 0).actions);
}

void test_wifistate_a_trial_from_idle_that_joins_makes_the_network_current(void)
{
    wifistate_init(&m);
    wifi_step_t out = wifistate_step(&m, WE_TRIAL_BEGIN, 0);
    assert_lacks(out, WA_DISCONNECT);
    wifistate_step(&m, WE_GOT_IP, 1);
    TEST_ASSERT_EQUAL(WS_CONNECTED, m.state);
    // A later trial that fails goes back to the network that joined.
    wifistate_step(&m, WE_TRIAL_BEGIN, 1);
    wifistate_step(&m, WE_TRIAL_TIMEOUT, 0);
    TEST_ASSERT_EQUAL(WS_RESTORING, m.state);
}

void test_wifistate_a_trial_or_apply_during_a_trial_or_restore_is_refused(void)
{
    const wifi_conn_state_t busy[] = {WS_TRIAL, WS_RESTORING};
    for (size_t i = 0; i < 2; i++) {
        reach(busy[i]);
        wifi_step_t out = wifistate_step(&m, WE_TRIAL_BEGIN, 1);
        TEST_ASSERT_EQUAL_HEX32(WA_REFUSED, out.actions);
        TEST_ASSERT_EQUAL(WF_BUSY, out.fail);
        out = wifistate_step(&m, WE_APPLY, 0);
        TEST_ASSERT_EQUAL_HEX32(WA_REFUSED, out.actions);
        TEST_ASSERT_EQUAL(busy[i], m.state);
    }
}

void test_wifistate_a_disconnect_reason_8_from_a_connected_board_is_a_drop(void)
{
    reach(WS_CONNECTED);
    const wifi_step_t out = wifistate_step(&m, WE_DISCONNECTED, R_ASSOC_LEAVE);
    TEST_ASSERT_EQUAL_HEX32(WA_ARM_RETRY, out.actions);
    TEST_ASSERT_EQUAL(WS_BACKOFF, m.state);
}

void test_wifistate_apply_from_connected_leaves_and_rejoins(void)
{
    reach(WS_CONNECTED);
    wifi_step_t out = wifistate_step(&m, WE_APPLY, 0);
    assert_has(out, WA_STOP_RETRY | WA_DISCONNECT | WA_SET_CURRENT_CONFIG | WA_CONNECT);
    TEST_ASSERT_EQUAL(WS_CONNECTING, m.state);
    // Its own disconnect arrives in CONNECTING and must not arm a retry over the connect in flight.
    out = wifistate_step(&m, WE_DISCONNECTED, R_ASSOC_LEAVE);
    TEST_ASSERT_EQUAL_HEX32(0, out.actions);
    TEST_ASSERT_EQUAL(WS_CONNECTING, m.state);
}

void test_wifistate_names_and_fail_text(void)
{
    TEST_ASSERT_EQUAL_STRING("trial", wifistate_name(WS_TRIAL));
    TEST_ASSERT_EQUAL_STRING("wrong password?", wifistate_fail_text(WF_WRONG_PASSWORD));
    TEST_ASSERT_EQUAL_STRING("network not found", wifistate_fail_text(WF_NOT_FOUND));
    TEST_ASSERT_EQUAL_STRING("timed out", wifistate_fail_text(WF_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("", wifistate_fail_text(WF_NONE));
}

/* ---- The scan list ---- */

static wifiscan_entry_t list[WIFISCAN_MAX];

void test_wifiscan_sorts_by_signal_and_skips_hidden_networks(void)
{
    size_t n = 0;
    n = wifiscan_add(list, n, WIFISCAN_MAX, "weak", -80, true);
    n = wifiscan_add(list, n, WIFISCAN_MAX, "", -30, true);
    n = wifiscan_add(list, n, WIFISCAN_MAX, "strong", -40, false);
    n = wifiscan_add(list, n, WIFISCAN_MAX, "middle", -60, true);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_STRING("strong", list[0].ssid);
    TEST_ASSERT_FALSE(list[0].secure);
    TEST_ASSERT_EQUAL_STRING("middle", list[1].ssid);
    TEST_ASSERT_EQUAL_STRING("weak", list[2].ssid);
}

void test_wifiscan_a_network_seen_twice_keeps_its_stronger_reading(void)
{
    size_t n = 0;
    n = wifiscan_add(list, n, WIFISCAN_MAX, "home", -70, true);
    n = wifiscan_add(list, n, WIFISCAN_MAX, "other", -50, true);
    n = wifiscan_add(list, n, WIFISCAN_MAX, "home", -45, true);  // stronger: moves up
    n = wifiscan_add(list, n, WIFISCAN_MAX, "other", -90, true); // weaker: ignored
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("home", list[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-45, list[0].rssi);
    TEST_ASSERT_EQUAL_STRING("other", list[1].ssid);
    TEST_ASSERT_EQUAL_INT8(-50, list[1].rssi);
}

void test_wifiscan_a_full_list_drops_its_weakest(void)
{
    size_t n = 0;
    char name[8];
    for (int i = 0; i < WIFISCAN_MAX; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        n = wifiscan_add(list, n, WIFISCAN_MAX, name, -40 - i, true); // n0 strongest .. n14 weakest
    }
    TEST_ASSERT_EQUAL_size_t(WIFISCAN_MAX, n);

    n = wifiscan_add(list, n, WIFISCAN_MAX, "weaker", -100, true); // weaker than all: not added
    TEST_ASSERT_EQUAL_size_t(WIFISCAN_MAX, n);
    TEST_ASSERT_EQUAL_STRING("n14", list[WIFISCAN_MAX - 1].ssid);

    n = wifiscan_add(list, n, WIFISCAN_MAX, "newer", -41, true); // ties go after the older entry; n14 falls off
    TEST_ASSERT_EQUAL_size_t(WIFISCAN_MAX, n);
    TEST_ASSERT_EQUAL_STRING("n0", list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("n1", list[1].ssid);
    TEST_ASSERT_EQUAL_STRING("newer", list[2].ssid);
    TEST_ASSERT_EQUAL_STRING("n13", list[WIFISCAN_MAX - 1].ssid);
}

void test_wifiscan_printable_replaces_other_bytes_and_cuts_to_the_buffer(void)
{
    char out[8];
    wifiscan_printable(out, sizeof(out), "ab\x01\xC3\xA9z");
    TEST_ASSERT_EQUAL_STRING("ab???z", out);
    wifiscan_printable(out, sizeof(out), "abcdefghij");
    TEST_ASSERT_EQUAL_STRING("abcdefg", out);
}

void test_wifistate_run(void)
{
    RUN_TEST(test_wifistate_boot_connects_and_a_drop_arms_the_retry);
    RUN_TEST(test_wifistate_a_retry_tick_outside_backoff_does_nothing);
    RUN_TEST(test_wifistate_a_retry_tick_during_a_scan_waits_another_period);
    RUN_TEST(test_wifistate_a_trial_from_backoff_cancels_the_retry_and_never_arms_it);
    RUN_TEST(test_wifistate_a_trial_from_connected_leaves_the_old_network_first);
    RUN_TEST(test_wifistate_our_own_disconnect_is_not_a_trial_failure);
    RUN_TEST(test_wifistate_a_got_ip_for_another_ssid_does_not_end_the_trial);
    RUN_TEST(test_wifistate_two_wrong_password_disconnects_end_the_trial_and_restore);
    RUN_TEST(test_wifistate_one_wrong_password_and_another_failure_keep_trying);
    RUN_TEST(test_wifistate_two_network_not_found_end_the_trial);
    RUN_TEST(test_wifistate_the_20_s_timeout_restores);
    RUN_TEST(test_wifistate_a_timeout_after_one_wrong_password_names_the_password);
    RUN_TEST(test_wifistate_a_stale_timeout_outside_a_trial_does_nothing);
    RUN_TEST(test_wifistate_a_restore_failure_falls_to_backoff_with_the_old_credentials);
    RUN_TEST(test_wifistate_the_old_network_coming_back_ends_the_restore);
    RUN_TEST(test_wifistate_a_trial_with_no_old_network_ends_in_idle);
    RUN_TEST(test_wifistate_a_trial_from_idle_that_joins_makes_the_network_current);
    RUN_TEST(test_wifistate_a_trial_or_apply_during_a_trial_or_restore_is_refused);
    RUN_TEST(test_wifistate_a_disconnect_reason_8_from_a_connected_board_is_a_drop);
    RUN_TEST(test_wifistate_apply_from_connected_leaves_and_rejoins);
    RUN_TEST(test_wifistate_names_and_fail_text);
    RUN_TEST(test_wifiscan_sorts_by_signal_and_skips_hidden_networks);
    RUN_TEST(test_wifiscan_a_network_seen_twice_keeps_its_stronger_reading);
    RUN_TEST(test_wifiscan_a_full_list_drops_its_weakest);
    RUN_TEST(test_wifiscan_printable_replaces_other_bytes_and_cuts_to_the_buffer);
}
