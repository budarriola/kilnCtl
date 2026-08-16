# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# App/ holds app_main() in place of the conventional "main" component, and
# App/drivers is its own sub-component (DcDac, i2c/spi/uart owners); both
# need to be pointed to explicitly since neither lives under main/ or
# components/ where the build would find them automatically.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/App"
    "${CMAKE_CURRENT_LIST_DIR}/App/drivers"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(UnitTest)
