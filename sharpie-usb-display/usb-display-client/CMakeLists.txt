cmake_minimum_required(VERSION 3.13...3.27)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD sharpie) # use sharpie.h declared in this directory


# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(sharpie-usb-display-client)



# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(sharpie-usb-display-client
  sharpie-usb-display-client.c
  usb_descriptors.c
  
  zstd/lib/common/debug.c
  zstd/lib/common/entropy_common.c
  zstd/lib/common/error_private.c
  zstd/lib/common/fse_decompress.c
  zstd/lib/common/threading.c
  zstd/lib/common/xxhash.c
  zstd/lib/common/zstd_common.c

  zstd/lib/decompress/huf_decompress.c
  zstd/lib/decompress/zstd_ddict.c
  zstd/lib/decompress/zstd_decompress.c
  zstd/lib/decompress/zstd_decompress_block.c
)

# let tinyusb see tusb_config.h
target_include_directories(sharpie-usb-display-client
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/zstd/lib
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/zstd/lib/common
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}/zstd/lib/decompress)

# only enable the parts of zstd that we need
add_compile_definitions(ZSTD_LIB_COMPRESSION=0)
add_compile_definitions(ZSTD_LIB_DEPRECATED=0)


pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-vertical.pio)
pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-gen.pio)
pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-horiz-data.pio)
## PIO code for partial updates as well
#pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-partial-gck.pio)
#pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-partial-intb-gsp.pio)
#pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-partial-gck-end.pio)
#pico_generate_pio_header(sharpie-usb-display-client ${CMAKE_CURRENT_LIST_DIR}/sharpie-partial-horiz-data.pio)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(sharpie-usb-display-client pico_stdlib pico_multicore
  hardware_uart hardware_dma hardware_pio hardware_pwm 
  tinyusb_device tinyusb_board cmsis_core)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(sharpie-usb-display-client)
