cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == Raspberry Pi Pico VS Code extension boilerplate (harmless without it) ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ==========================================================================

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(rtltcp_rp2350 C CXX ASM)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Patched TinyUSB host controller driver (bulk pipe on EPX, see tinyusb_patch/)
# ---------------------------------------------------------------------------
if (TARGET tinyusb_host_base)
    get_target_property(_tuh_srcs tinyusb_host_base INTERFACE_SOURCES)
    set(_new_srcs "")
    foreach(_s IN LISTS _tuh_srcs)
        if (_s MATCHES "portable/raspberrypi/rp2040/hcd_rp2040\\.c$")
            list(APPEND _new_srcs ${CMAKE_CURRENT_LIST_DIR}/tinyusb_patch/hcd_rp2040.c)
        elseif (_s MATCHES "portable/raspberrypi/rp2040/rp2040_usb\\.c$")
            list(APPEND _new_srcs ${CMAKE_CURRENT_LIST_DIR}/tinyusb_patch/rp2040_usb.c)
        else()
            list(APPEND _new_srcs ${_s})
        endif()
    endforeach()
    set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_new_srcs}")
    # the patched files include the original private headers
    target_include_directories(tinyusb_host_base INTERFACE
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040)
    message(STATUS "TinyUSB hcd_rp2040.c / rp2040_usb.c replaced by tinyusb_patch/")
else()
    message(FATAL_ERROR "tinyusb_host_base not found: initialise the pico-sdk tinyusb submodule")
endif()

# ---------------------------------------------------------------------------
add_executable(rtltcp_rp2350
    src/main.c
    src/usb_host.c
    src/rtl2832u.c
    src/net_server.c
    src/ring.c
    src/leds.c
    tuner/tuner_r82xx.c
)

pico_set_program_name(rtltcp_rp2350 "rtltcp_rp2350")
pico_set_program_version(rtltcp_rp2350 "1.0.0")

# console is UART0 (USB-C is the SDR host port)
pico_enable_stdio_uart(rtltcp_rp2350 0)
pico_enable_stdio_usb(rtltcp_rp2350 0)

add_subdirectory(lib/Config)
add_subdirectory(lib/ethchip)
add_subdirectory(lib/WS2812)

target_include_directories(rtltcp_rp2350 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/tuner
)

target_compile_options(rtltcp_rp2350 PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_compile_definitions(rtltcp_rp2350 PRIVATE
    PICO_FLASH_SIZE_BYTES=16777216
    CFG_TUSB_CONFIG_FILE="tusb_config.h"
    PICO_STDIO_UART=1
    PICO_CORE1_STACK_SIZE=0x1000
)

target_link_libraries(rtltcp_rp2350
    pico_stdlib
    pico_stdio_uart
    pico_multicore
    pico_unique_id
    hardware_clocks
    hardware_uart
    tinyusb_host
    ethchip
    DHCP_FILES
    PORT_ETHCHIP_FILES
    PORT_COMMON_FILES
    TIMER_FILES
    WS2812
    Config
)

pico_add_extra_outputs(rtltcp_rp2350)
