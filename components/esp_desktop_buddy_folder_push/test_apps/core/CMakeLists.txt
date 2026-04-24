cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../components")
set(COMPONENTS main esp_desktop_buddy_folder_push)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_desktop_buddy_folder_push_tests)
