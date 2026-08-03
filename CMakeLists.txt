# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Layered component layout: app/ and drivers/ are container directories
# whose direct subdirectories (each with a CMakeLists.txt) are components.
set(EXTRA_COMPONENT_DIRS
    components/app
    components/drivers
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaomiao)
