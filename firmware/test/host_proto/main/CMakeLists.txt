# The ../../../main files are the firmware's, compiled as is. Only the tests are host-specific. cardfs.c runs on
# the host's own file system here (logic only); host_fatfs runs it on FatFs.
idf_component_register(SRCS "test_proto.c" "test_grid.c" "test_pageset.c" "test_hoststat.c" "test_logring.c"
                            "test_diag.c" "test_uimode.c" "test_sleepcfg.c" "test_feedfmt.c" "test_cardfs.c"
                            "test_wifitxt.c" "test_svc.c" "test_kbmap.c" "test_wifistate.c" "../../../main/proto.c"
                            "../../../main/font_ids.c" "../../../main/grid.c" "../../../main/pageset.c"
                            "../../../main/hoststat.c" "../../../main/logring.c" "../../../main/diag.c"
                            "../../../main/uimode.c" "../../../main/sleepcfg.c" "../../../main/feedfmt.c"
                            "../../../main/cardfs.c" "../../../main/wifitxt.c" "../../../main/svc.c"
                            "../../../main/kbmap.c" "../../../main/wifistate.c"
                       INCLUDE_DIRS "../../../main"
                       PRIV_REQUIRES unity mbedtls log cjson esp_rom freertos)
