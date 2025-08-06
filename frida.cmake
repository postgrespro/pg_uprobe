# add frida as external project
set(FRIDA_DOWNLOAD_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/frida)

set(FRIDA_DOWNLOAD_URL_PREFIX "" CACHE STRING "The prefix added to the frida download url. For example, https://ghproxy.com/")

message(STATUS "System Name: ${CMAKE_SYSTEM_NAME}")
message(STATUS "System Version: ${CMAKE_SYSTEM_VERSION}")
message(STATUS "System Processor: ${CMAKE_SYSTEM_PROCESSOR}")

set(FRIDA_OS_ARCH_RAW "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
string(TOLOWER ${FRIDA_OS_ARCH_RAW} FRIDA_OS_ARCH)
set(FRIDA_VERSION "17.1.5")

message(STATUS "Using frida: arch=${FRIDA_OS_ARCH}, version=${FRIDA_VERSION}")

if(${FRIDA_OS_ARCH} STREQUAL "linux-x86_64")
  set(FRIDA_CORE_DEVKIT_SHA256 "14f533c7aa45e3d9ef9a711833a436d17bf20251c5579ee498ffb907b2a0e127")
  set(FRIDA_GUM_DEVKIT_SHA256 "46bdd9a463b36127a9f5d2a9c770aa738d392c723f213436ec939096125a7a09")
elseif(${FRIDA_OS_ARCH} STREQUAL "linux-aarch64")
  set(FRIDA_CORE_DEVKIT_SHA256 "df8b4ad168e21398548a407bc6fbd68121ef1cc92c455a11edbc85c423101cfe")
  set(FRIDA_GUM_DEVKIT_SHA256 "3aea84ef12537415e511971c7b04d89f7d539c669d0e730c0662779c75667261")
  # Cmake uses aarch64, but frida uses arm64
  set(FRIDA_OS_ARCH "linux-arm64")
elseif(${FRIDA_OS_ARCH} MATCHES "linux-arm.*")
  set(FRIDA_CORE_DEVKIT_SHA256 "b0113039b83a2542a1dca122cc29bc95e62ceced452d351a0e9b5452d5ade9e9")
  set(FRIDA_GUM_DEVKIT_SHA256 "6faa9cbd76a06c40f43f40c5d4bf3138566e40fe0438c616e1b7f0b6ccf4f0dc")
  # Frida only has armhf builds..
  set(FRIDA_OS_ARCH "linux-armhf")
elseif(${FRIDA_OS_ARCH} MATCHES "darwin-arm.*")
  set(FRIDA_CORE_DEVKIT_SHA256 "674e1deb0a2ce28456755bdfa00fb4b866f651afff84bb3e0eb349f52ec8b90b")
  set(FRIDA_GUM_DEVKIT_SHA256 "1d148cbcf1ac32611417beef728864bcdb8b81b7479830b187f3981a4289d640")
  # for macos-arm m* chip series
  set(FRIDA_OS_ARCH "macos-arm64e")
elseif(${FRIDA_OS_ARCH} STREQUAL "freebsd-amd64")
  set(FRIDA_CORE_DEVKIT_SHA256 "8b85853d5cde9cdd3eb992bc74e710f236a5a329d90b0f2caa55556351c7113a")
  set(FRIDA_GUM_DEVKIT_SHA256 "e3a3a74818aab8d268bf4eb4ddadde6ed4e720985b5e7ade53e8c9c8f6b34e5d")
  # Cmake uses freebsd-amd64 but frida has freebsd-x86_64
  set(FRIDA_OS_ARCH "freebsd-x86_64")
else()
  message(FATAL_ERROR "Unsupported frida arch ${FRIDA_OS_ARCH}")
endif()

set(FRIDA_CORE_FILE_NAME "frida-core-devkit-${FRIDA_VERSION}-${FRIDA_OS_ARCH}.tar.xz")
set(FRIDA_GUM_FILE_NAME "frida-gum-devkit-${FRIDA_VERSION}-${FRIDA_OS_ARCH}.tar.xz")
set(FRIDA_CORE_DEVKIT_URL "${FRIDA_DOWNLOAD_URL_PREFIX}https://github.com/frida/frida/releases/download/${FRIDA_VERSION}/${FRIDA_CORE_FILE_NAME}")
set(FRIDA_GUM_DEVKIT_URL "${FRIDA_DOWNLOAD_URL_PREFIX}https://github.com/frida/frida/releases/download/${FRIDA_VERSION}/${FRIDA_GUM_FILE_NAME}")

set(FRIDA_CORE_DEVKIT_PATH ${FRIDA_DOWNLOAD_LOCATION}/${FRIDA_CORE_FILE_NAME})
set(FRIDA_GUM_DEVKIT_PATH ${FRIDA_DOWNLOAD_LOCATION}/${FRIDA_GUM_FILE_NAME})

set(FRIDA_CORE_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/FridaCore-prefix/src/FridaCore)
set(FRIDA_GUM_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/FridaGum-prefix/src/FridaGum)

# if file exists, skip download
if(NOT EXISTS ${FRIDA_CORE_DEVKIT_PATH})
  message(STATUS "Downloading Frida Core Devkit")
  set(FRIDA_CORE_DOWNLOAD_URL ${FRIDA_CORE_DEVKIT_URL})
else()
  message(STATUS "Frida Core Devkit already downloaded")
  set(FRIDA_CORE_DOWNLOAD_URL ${FRIDA_CORE_DEVKIT_PATH})
endif()

# if file exists, skip download
if(NOT EXISTS ${FRIDA_GUM_DEVKIT_PATH})
  message(STATUS "Downloading Frida GUM Devkit")
  set(FRIDA_GUM_DOWNLOAD_URL ${FRIDA_GUM_DEVKIT_URL})
else()
  message(STATUS "Frida GUM Devkit already downloaded")
  set(FRIDA_GUM_DOWNLOAD_URL ${FRIDA_GUM_DEVKIT_PATH})
endif()

message(STATUS "Downloading FridaCore from ${FRIDA_CORE_DOWNLOAD_URL}")
include(ExternalProject)
ExternalProject_Add(FridaCore
  URL ${FRIDA_CORE_DOWNLOAD_URL}
  DOWNLOAD_DIR ${FRIDA_DOWNLOAD_LOCATION}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${FRIDA_CORE_INSTALL_DIR}/libfrida-core.a
  URL_HASH SHA256=${FRIDA_CORE_DEVKIT_SHA256}
)

message(STATUS "Downloading FridaGum from ${FRIDA_GUM_DOWNLOAD_URL}")
ExternalProject_Add(FridaGum
  URL ${FRIDA_GUM_DOWNLOAD_URL}
  DOWNLOAD_DIR ${FRIDA_DOWNLOAD_LOCATION}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${FRIDA_GUM_INSTALL_DIR}/libfrida-gum.a
  URL_HASH SHA256=${FRIDA_GUM_DEVKIT_SHA256}
)
