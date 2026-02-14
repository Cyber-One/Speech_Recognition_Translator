cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK
include(pico_sdk_import.cmake)

project(Speech_Recognition_Translator C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# FatFs library
add_library(fatfs STATIC
    third_party/fatfs/source/ff.c
    third_party/fatfs/source/ffsystem.c
    third_party/fatfs/source/ffunicode.c
)
target_include_directories(fatfs PUBLIC
    third_party/fatfs/source
)

# Create executable
add_executable(Speech_Recognition_Translator
    speech_recognition_translator.c
    sd_driver.c
)

# Pull in common dependencies
target_link_libraries(Speech_Recognition_Translator
    pico_stdlib
    hardware_i2c
    hardware_spi
    hardware_gpio
    hardware_uart
    fatfs
)

# Include FatFs headers
target_include_directories(Speech_Recognition_Translator PRIVATE
    third_party/fatfs/source
)

# Enable USB output, disable UART output by default (UART used for TTL output)
pico_enable_stdio_usb(Speech_Recognition_Translator 1)
pico_enable_stdio_uart(Speech_Recognition_Translator 0)

# Create UF2 file
pico_add_extra_outputs(Speech_Recognition_Translator)
