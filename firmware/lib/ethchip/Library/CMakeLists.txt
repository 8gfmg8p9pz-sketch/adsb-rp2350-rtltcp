set(NETLIB_DIR ${CMAKE_CURRENT_LIST_DIR}/ioLibrary_Driver)

add_library(ethchip STATIC)

target_sources(ethchip PRIVATE
    ${NETLIB_DIR}/Ethernet/socket.c
    ${NETLIB_DIR}/Ethernet/ethchip_conf.c
    ${NETLIB_DIR}/Ethernet/W6300/w6300.c
)

target_include_directories(ethchip PUBLIC
    ${NETLIB_DIR}/Ethernet
    ${NETLIB_DIR}/Ethernet/W6300
)

# DHCP
add_library(DHCP_FILES STATIC)
target_sources(DHCP_FILES PRIVATE
    ${NETLIB_DIR}/Internet/DHCP/dhcp.c
)
target_include_directories(DHCP_FILES PUBLIC
    ${NETLIB_DIR}/Internet/DHCP
)
target_link_libraries(DHCP_FILES PUBLIC ethchip)
