set(SOURCES
    "init_fs.c"
)

idf_component_register(
    SRCS ${SOURCES}
    PRIV_REQUIRES spiffs
    INCLUDE_DIRS 
        "include"
)

