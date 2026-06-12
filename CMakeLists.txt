cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_ssh_term)

# Mandatory local patch to the fetched esp_hosted component (defers its
# auto-init; see tools/patch_esp_hosted.py). Runs at configure time, after
# the component manager has resolved managed_components.
execute_process(
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_esp_hosted.py
    RESULT_VARIABLE TAB5_PATCH_RC
)
if(NOT TAB5_PATCH_RC EQUAL 0)
    message(FATAL_ERROR "esp_hosted patch failed - see tools/patch_esp_hosted.py")
endif()
