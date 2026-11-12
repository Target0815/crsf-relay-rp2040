cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
  set(PICO_BOARD waveshare_rp2040_zero)
endif()

include(pico_sdk_import.cmake)
project(crsf_relay C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(crsf_relay
  src/main.cpp
  src/core.cpp
  src/mlrs.cpp
  src/ports.cpp
)
target_include_directories(crsf_relay PRIVATE src)
target_compile_options(crsf_relay PRIVATE -Wall -Wextra -Wno-psabi)
pico_generate_pio_header(crsf_relay ${CMAKE_CURRENT_LIST_DIR}/src/hd_uart.pio)
pico_generate_pio_header(crsf_relay ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

foreach(p PIN_IN_TX PIN_IN_RX PIN_RELAY_TX PIN_RELAY_RX PIN_WS2812 PIN_VSENSE VSENSE_R1 VSENSE_R2)
  if(DEFINED ${p})
    target_compile_definitions(crsf_relay PRIVATE ${p}=${${p}})
  endif()
endforeach()

target_compile_definitions(crsf_relay PRIVATE
  PICO_STDIO_USB_STDOUT_TIMEOUT_US=2000
  PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
)

target_link_libraries(crsf_relay
  pico_stdlib pico_flash
  hardware_pio hardware_dma hardware_uart hardware_adc hardware_flash hardware_watchdog hardware_sync
)
pico_enable_stdio_usb(crsf_relay 1)
pico_enable_stdio_uart(crsf_relay 0)
pico_set_program_name(crsf_relay "crsf-relay")
pico_set_program_description(crsf_relay "CRSF relay (port of RC-Relay32 by Wilhelm Meier)")
pico_set_program_version(crsf_relay "1.0.6")
pico_set_program_url(crsf_relay "https://github.com/wimalopaan/wmucpp/tree/master/boards/rcRelay32")
pico_add_extra_outputs(crsf_relay)
