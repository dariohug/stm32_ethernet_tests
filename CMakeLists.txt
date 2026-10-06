cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME stm32_ethernet_test)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Places ETH DMA descriptors/Rx pool in D2 SRAM; CubeMX's own linker script
# doesn't know about these sections, so this stays user-owned (see the file).
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T ${CMAKE_SOURCE_DIR}/cmake/eth_ram_d2.ld
)

# D2 SRAM (0x30000000) is unclocked out of reset: SystemInit() only sets
# RCC_AHB2ENR's SRAM1/SRAM2 enable bits when DATA_IN_D2_SRAM is defined, and
# CubeMX never defines it. The ETH DMA descriptors and Rx pool live there (see
# the linker fragment above), so without this the MAC/PHY still come up but the
# descriptors never retain what's written to them, and the first received packet
# faults in ethernetif_input(). Defined on the stm32cubemx INTERFACE target
# rather than the executable because CubeMX compiles system_stm32h7xx.c into
# STM32_Drivers, which links that interface PUBLIC.
target_compile_definitions(stm32cubemx INTERFACE DATA_IN_D2_SRAM)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/cmd_server.c
    Core/Src/eth_log.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
