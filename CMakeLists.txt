#
# Author: Milan M.
# Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
#

cmake_minimum_required(VERSION 3.10)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake;${CMAKE_MODULE_PATH}")
project(libwsc 
  VERSION 1.1.0
  LANGUAGES C CXX
)

set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_TLS "Enable TLS/SSL support using OpenSSL" OFF)
option(LIBWSC_USE_DEBUG "Enable debug (verbose) output" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" OFF)

set(LIBEVENT_COMPONENTS libevent pthreads)

if(USE_TLS)
    list(APPEND LIBEVENT_COMPONENTS openssl)
    find_package(OpenSSL REQUIRED)
endif()

find_package(Libevent REQUIRED COMPONENTS ${LIBEVENT_COMPONENTS})
find_package(ZLIB REQUIRED)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (UNIX)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
endif()

set (LIBWSC_SOURCES 
  src/WebSocketClient.cpp 
  src/WebSocketContext.cpp 
  src/Utf8Validator.cpp 
  src/WebSocketReceiver.cpp 
  src/base64.cpp)
set (LIBWSC_HEADERS 
  src/WebSocketClient.h
  src/Logger.h
  src/Utf8Validator.h
  src/base64.h
  src/WebSocketHeaders.h
  src/WebSocketTLSOptions.h
  src/WebSocketContext.h
  src/IWebSocketSinks.h
  src/WebSocketReceiver.h)

if (USE_TLS)
    list(APPEND LIBWSC_SOURCES src/WebSocketTLSContext.cpp)
    list(APPEND LIBWSC_HEADERS src/WebSocketTLSContext.h)
endif()

add_library(libwsc 
    ${LIBWSC_SOURCES}
    ${LIBWSC_HEADERS}
)

target_include_directories(libwsc
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/libwsc>
  PRIVATE
    ${LIBEVENT_INCLUDE_DIRS}
)

target_link_libraries(libwsc
  PRIVATE
    ${LIBEVENT_LIBRARIES}
    ZLIB::ZLIB
)

if (LIBWSC_USE_DEBUG)
    target_compile_definitions(libwsc PRIVATE LIBWSC_USE_DEBUG)
endif()

if (USE_TLS)
    target_compile_definitions(libwsc PRIVATE USE_TLS)
    target_link_libraries(libwsc PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# some size-specific options
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(libwsc PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(libwsc PRIVATE -Wl,--gc-sections)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(libwsc PRIVATE -Os)
  endif()
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
  add_custom_command(TARGET libwsc POST_BUILD
    COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:libwsc>
    VERBATIM
  )
endif()

set_target_properties(libwsc PROPERTIES 
  LINK_FLAGS_RELEASE -s
  PREFIX ""
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(TARGETS libwsc
  EXPORT libwscTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/WebSocketClient.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/WebSocketHeaders.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/WebSocketTLSOptions.h
  DESTINATION include/libwsc
)

install(EXPORT libwscTargets
  FILE libwscConfig.cmake
  NAMESPACE libwsc::
  DESTINATION lib/cmake/libwsc
)