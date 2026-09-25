
set(NETLIB_DIR ${CMAKE_CURRENT_LIST_DIR}/../Library/ioLibrary_Driver)
set(PORT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(PORT_ETHCHIP_SRC_DIR ${PORT_DIR}/ioLibrary_Driver/src)


# ioLibrary_Driver
add_library(PORT_ETHCHIP_FILES STATIC)

target_sources(PORT_ETHCHIP_FILES PUBLIC
        ${PORT_ETHCHIP_SRC_DIR}/ethchip_spi.c
        ${PORT_ETHCHIP_SRC_DIR}/ethchip_gpio_irq.c
)

target_include_directories(PORT_ETHCHIP_FILES PUBLIC
        ${NETLIB_DIR}/Ethernet
        ${PORT_DIR}/ioLibrary_Driver/inc
        ${PORT_DIR}
)

target_link_libraries(PORT_ETHCHIP_FILES PUBLIC
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_clocks
        Config
)

# timer
add_library(TIMER_FILES STATIC)

target_sources(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer/timer.c
)

target_include_directories(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer
)

target_link_libraries(TIMER_FILES PRIVATE
        pico_stdlib      
)

# port_common
add_library(PORT_COMMON_FILES STATIC)

target_include_directories(PORT_COMMON_FILES PUBLIC
        ${PORT_DIR}
)

target_link_libraries(PORT_COMMON_FILES PUBLIC
        pico_stdlib
        hardware_clocks
        hardware_spi
        hardware_dma
)

