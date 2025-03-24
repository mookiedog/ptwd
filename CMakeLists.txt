# This is a generic CMakeLists file to build a hello-world kind of application
# for a board in the Raspberry Pi Pico family.

cmake_minimum_required(VERSION 3.13)

include(FetchContent)

# It is assumed that there is a 'projects' dir somewhere that is used as the root directory
# for everything this project needs. At a minimum, the directory hierarchy must look like this:
#   projects/
#   ├── pico-sdk
#   ├── pico-tool
#   └── hello-world

set(PROJECTS_PATH ${CMAKE_SOURCE_DIR}/..)

# The Pico SDK is too big to download for every project on this machine that might want to use it.
# Instead, it is assumed that a user has downloaded a single, sharable copy to a known location.
# The projects directory structure is set up to allow for multiple versions of the SDK to exist beside each other.
# All versions will be located at ~/projects/pico-sdk/<version-tag>, like this:
#   projects/
#   └── pico-sdk
#       ├── 1.5.1
#       ├── 2.0.0
#       └── 2.1.1

# Select the SDK version. This variable gets passed into the WP and EP external projects.
# At the moment, both EP and WP use the same SDK version.
# It would be possible for the WP and EP to use different versions of the SDK, should that need arise.
set(PICO_SDK_VERSION 2.1.1)
set(PICO_SDK_PATH ${PROJECTS_PATH}/pico-sdk/${PICO_SDK_VERSION})

# If this path is specified, the SDK init process will make sure that the picotool sources will get downloaded to this path,
# that the picotool gets built, and that the various sub-projects in this superbuild will know where to find the picotool executable.
set(PICOTOOL_FETCH_FROM_GIT_PATH ${PROJECTS_PATH}/picotool)

set(PICO_ARM_TOOLCHAIN_PATH   ${CMAKE_SOURCE_DIR}/cmake/toolchains/arm-none-eabi.cmake)
set(CMAKE_TOOLCHAIN_FILE ${PICO_ARM_TOOLCHAIN_PATH})

# --------------------------------------------------------------------------------------------------
# These PICO_* definitions need to occur before including pico_sdk_import.cmake:

# Define the Raspberry Pi board file that the SDK should use for this build.
# Can be any of [pico, pico_w, pico2, pico2_w]:
set(PICO_BOARD pico2)

# Uncommenting this next line will allow RP2040 chips to run at 200 MHz now instead of 125/133 MHz.
# There is a proportionate increase in power consumption, if that matters to your project.
# As of 3/2025 the fastest an RP2350 will officially run is still 150 MHz.
#set(PICO_USE_FASTEST_SUPPORTED_CLOCK 1)

# Based on the board definition, define the appropriate platform and cpu:
if (${PICO_BOARD} STREQUAL "pico")
  set(PICO_PLATFORM rp2040)
  set(CPU cortex-m0)
elseif (${PICO_BOARD} STREQUAL "pico_w")
  set(PICO_PLATFORM rp2040)
  set(CPU cortex-m0)
elseif (${PICO_BOARD} STREQUAL "pico2")
  set(PICO_PLATFORM rp2350)
  set(CPU cortex-m33)
elseif (${PICO_BOARD} STREQUAL "pico2_w")
  set(PICO_PLATFORM rp2350)
  set(CPU cortex-m33)
else()
  message(FATAL_ERROR "Unknown board type!")
endif()


# As per SDK doc, this include needs to happen before project()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH ${PICO_SDK_PATH}/FreeRTOS-Kernel)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

# Prevent errors that will be generated if CMake tries to test the cross-compiler:
set (CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

project(hello-world
  LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# The toolchain file explained where to find the cross-compiler.
# We need to tell that cross-compiler a bit more about how to compile the code for this project.

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -mcpu=${CPU} -mthumb")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=${CPU} -mthumb")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=${CPU} -mthumb")

# This is required to avoid a linker error: undefined reference to '__cxa_guard_acquire'
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-threadsafe-statics")

# Disable C++ exception processing for small embedded projects
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")

# Turn off RTTI (C++ Run Time Typing Information): not needed for small embedded projects
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

# This is probably the wrong way to do this...
# For now, compile all the Cortex code with -Og for optimized, but pretty debuggable.
# -O2 generates much better code, but is harder to follow when debugging.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Og")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Og")

pico_sdk_init()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  #${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/include
  #${PROJECTS_PATH}/FreeRTOS-Kernel/include/
  #${PICO_SDK_PATH}/src/common/pico_base_headers/include/
)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/Onewire.cpp
)

# Choose where our stdio output goes. It should only be RTT for this project.
pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_rtt(${PROJECT_NAME} 1)

add_subdirectory(${PROJECTS_PATH}/pico-examples/pio/onewire/onewire_library ${CMAKE_CURRENT_BINARY_DIR}/onewire_library)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pico_stdlib
    hardware_spi
    onewire_library
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap1
  )

# Can't do this because picotool invocation is broken regarding creation of uf2 files
# pico_add_extra_outputs(${PROJECT_NAME})
# Instead, we manually make the three files we want:
pico_add_dis_output(${PROJECT_NAME})
pico_add_map_output(${PROJECT_NAME})
pico_add_hex_output(${PROJECT_NAME})

# After the build completes, print some basic information regarding the build size
add_custom_command(
  TARGET ${PROJECT_NAME} POST_BUILD
  VERBATIM
  COMMAND ${CROSSCOMPILE_TOOL_PATH}/arm-none-eabi-size ${CMAKE_BINARY_DIR}/${PROJECT_NAME}
)
