cmake_minimum_required(VERSION 3.15)
set(CMAKE_C_STANDARD 99)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  cmake_policy(SET CMP0135 NEW)
endif()

project(
  "load_frida"
  VERSION 1.0
  LANGUAGES C
)

include(frida.cmake)