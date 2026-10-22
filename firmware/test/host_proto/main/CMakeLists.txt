# proto.c, font_ids.c, grid.c and pageset.c are the firmware's files, compiled as is. Only the tests are host-specific.
idf_component_register(SRCS "test_proto.c" "test_grid.c" "test_pageset.c" "../../../main/proto.c"
                            "../../../main/font_ids.c" "../../../main/grid.c" "../../../main/pageset.c"
                       INCLUDE_DIRS "../../../main"
                       PRIV_REQUIRES unity mbedtls log cjson)
