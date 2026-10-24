# proto.c, font_ids.c, grid.c, pageset.c, hoststat.c, logring.c, diag.c, uimode.c and sleepcfg.c are the firmware's files, compiled as is. Only the tests are host-specific.
idf_component_register(SRCS "test_proto.c" "test_grid.c" "test_pageset.c" "test_hoststat.c" "test_logring.c"
                            "test_diag.c" "test_uimode.c" "test_sleepcfg.c" "../../../main/proto.c"
                            "../../../main/font_ids.c" "../../../main/grid.c" "../../../main/pageset.c"
                            "../../../main/hoststat.c" "../../../main/logring.c" "../../../main/diag.c"
                            "../../../main/uimode.c" "../../../main/sleepcfg.c"
                       INCLUDE_DIRS "../../../main"
                       PRIV_REQUIRES unity mbedtls log cjson)
