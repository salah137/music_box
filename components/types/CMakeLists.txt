set(sources
    "local_types.c"
)

file(GLOB SOURCES *.c)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "include"
    
)