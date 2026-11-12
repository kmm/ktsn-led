cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build and link only what `main` depends on, so disabled features (e.g. WiFi) really drop out of the image.
idf_build_set_property(MINIMAL_BUILD ON)
project(ktsn-led)
