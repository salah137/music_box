# Get all .c files in the current directory automatically
set(sources 
    "drawing.c" 
    "i2c_connection.c")
    
file(GLOB SOURCES *.c)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "include"    # Public headers (other components can see these)
    REQUIRES driver esp_driver_i2c
)