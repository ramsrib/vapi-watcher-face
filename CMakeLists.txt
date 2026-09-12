# Vapi Watcher Face — an animated face on a SenseCAP Watcher that sees you and
# talks back through Vapi.
#
# The board support package and the Himax NPU client come from Seeed's
# SenseCAP-Watcher-Firmware, vendored under ../_sdk. Components are named one by
# one rather than adding the whole components/ directory: IDF builds everything
# it discovers there, and that tree contains `rlottie`, whose vendored
# CMakeLists declares `cmake_minimum_required(VERSION <3.5)` and is rejected
# outright by modern CMake. Naming components sidesteps it entirely.
cmake_minimum_required(VERSION 3.16)

# CMake 4 removed compatibility with `cmake_minimum_required(VERSION <3.5)`, and
# the SDK's vendored rlottie still declares one. rlottie arrives unavoidably —
# the BSP pins esp_lvgl_port by override_path, which pins the SDK's lvgl, which
# pins rlottie — so it cannot be scoped out. This is CMake's supported escape
# hatch for exactly that situation, and it keeps the SDK checkout unpatched.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(WATCHER_SDK "${CMAKE_CURRENT_LIST_DIR}/../_sdk/SenseCAP-Watcher-Firmware")
if(NOT EXISTS "${WATCHER_SDK}/components/sensecap-watcher")
    message(FATAL_ERROR
        "SenseCAP-Watcher-Firmware not found at ${WATCHER_SDK}.\n"
        "git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git "
        "and run: git submodule update --init --recursive")
endif()

# Note lvgl is absent: the SDK's vendored copy hard-depends on rlottie via
# override_path, and rlottie's own CMakeLists declares
# cmake_minimum_required(VERSION <3.5), which modern CMake refuses outright.
# We draw the face with LVGL primitives rather than Lottie, so upstream LVGL
# from the component registry (see main/idf_component.yml) does the job and
# leaves the SDK checkout pristine.
foreach(_c sensecap-watcher sscma_client esp_lvgl_port
           esp_io_expander_pca95xx_16bit esp_jpeg_simd knob)
    if(EXISTS "${WATCHER_SDK}/components/${_c}")
        list(APPEND EXTRA_COMPONENT_DIRS "${WATCHER_SDK}/components/${_c}")
    endif()
endforeach()

# --- .env support -----------------------------------------------------------
# Turns a gitignored .env into main/env_config.generated.h, which settings.h
# prefers over the menuconfig CONFIG_* values. Keeps WiFi and Vapi credentials
# out of sdkconfig and out of git. A generated header rather than -D defines,
# so values containing spaces, quotes or semicolons survive CMake intact.
set(VAPI_ENV_FILE   "${CMAKE_CURRENT_LIST_DIR}/.env")
set(VAPI_ENV_HEADER "${CMAKE_CURRENT_LIST_DIR}/main/env_config.generated.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VAPI_ENV_FILE}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/gen_env_header.py"
                    "${VAPI_ENV_FILE}" "${VAPI_ENV_HEADER}"
    RESULT_VARIABLE _vapi_env_rc)
if(NOT _vapi_env_rc EQUAL 0)
    message(WARNING "vapi: could not generate ${VAPI_ENV_HEADER} (rc=${_vapi_env_rc})")
elseif(EXISTS "${VAPI_ENV_FILE}")
    message(STATUS "vapi: applied config overrides from ${VAPI_ENV_FILE}")
endif()

project(vapi_watcher_face)
