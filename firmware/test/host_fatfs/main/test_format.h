#pragma once

// Format card's tests (test_format.c), run from app_main in test_cuts.c after the cut tests.
void test_format_layout_by_card_size(void);
void test_format_128_gib_exfat_card(void);
void test_format_8_gib_blank_card(void);
void test_format_gpt_card(void);
void test_format_cut_at_every_write(void);
