# Get all .c files in the current directory automatically
set(sources 
    "init_bt.c" 
    )
    
file(GLOB SOURCES *.c)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "include"    # Public headers (other components can see these)
    REQUIRES driver bt nvs_flash 
    PRIV_REQUIRES types
)