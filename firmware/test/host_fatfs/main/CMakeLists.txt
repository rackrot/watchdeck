# cardfs.c and cardfs_fat.c are the firmware's files, compiled as is, over IDF's own FatFs and VFS for the linux target.
idf_component_register(SRCS "test_cuts.c" "ramdisk.c" "fatwalk.c" "../../../main/cardfs.c" "../../../main/cardfs_fat.c"
                       INCLUDE_DIRS "../../../main"
                       PRIV_REQUIRES unity fatfs vfs mbedtls log esp_rom freertos)
