# proto.c is the firmware's file, compiled as is. Only the test is host-specific.
idf_component_register(SRCS "test_proto.c" "../../../main/proto.c"
                       INCLUDE_DIRS "../../../main"
                       PRIV_REQUIRES unity mbedtls log cjson)
