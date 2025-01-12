cmake_minimum_required(VERSION 3.13...3.27)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(hello_usb)



# initialize the Raspberry Pi Pico SDK
pico_sdk_init()



add_executable(sharpie-vertical
  main.c
)
pico_enable_stdio_usb(sharpie-vertical 1)
pico_enable_stdio_uart(sharpie-vertical 0)


pico_generate_pio_header(sharpie-vertical ${CMAKE_CURRENT_LIST_DIR}/sharpie-vertical.pio)
pico_generate_pio_header(sharpie-vertical ${CMAKE_CURRENT_LIST_DIR}/sharpie-gen.pio)
pico_generate_pio_header(sharpie-vertical ${CMAKE_CURRENT_LIST_DIR}/sharpie-horiz-data.pio)
# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(sharpie-vertical pico_stdlib hardware_pio hardware_dma)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(sharpie-vertical)
