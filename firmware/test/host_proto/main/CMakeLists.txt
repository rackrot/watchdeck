# proto.c and font_ids.c are the firmware's files, compiled as is. Only the test is host-specific.
idf_component_register(SRCS "test_proto.c" "../../../main/proto.c" "../../../main/font_ids.c"
                       INCLUDE_DIRS "../../../main"
                       PRIV_REQUIRES unity mbedtls log cjson)
