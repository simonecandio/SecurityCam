# Progetto Security Camera - ESP32-S3 Freenove
# Esame IoT e Programmazione Mobile
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(security_cam)
