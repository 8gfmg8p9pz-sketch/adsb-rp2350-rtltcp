# Search for all source files in the current directory
# and save the names to the DIR_WS2812_SRCS variable
aux_source_directory(. DIR_WS2812_SRCS)

# Generate link library
add_library(WS2812 ${DIR_WS2812_SRCS})

pico_generate_pio_header(WS2812 ${CMAKE_CURRENT_LIST_DIR}/WS2812.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR})

target_include_directories(WS2812 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(WS2812 PUBLIC 
    pico_stdlib
    hardware_pio
    )