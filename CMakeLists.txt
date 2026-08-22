# ESP-IDF 在引入 project.cmake 时读取 PROJECT_VER，并写入生成镜像的 app description。
# 每次发布到巴法云前手动递增此正整数版本号。
cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "2")

add_compile_options(-fdiagnostics-color=always)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(homeassistant)
