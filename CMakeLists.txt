cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Find libmoq
find_path(MOQ_INCLUDE_DIR moq.h
    PATHS
        "$ENV{HOME}/src/moq/target/include"
        "/home/ubuntu/src/moq/rs/target/include"
    NO_DEFAULT_PATH
)
find_library(MOQ_LIBRARY moq
    PATHS
        "$ENV{HOME}/src/moq/target/release"
        "/home/ubuntu/src/moq/rs/target/release"
    NO_DEFAULT_PATH
)

if(NOT MOQ_INCLUDE_DIR OR NOT MOQ_LIBRARY)
    message(FATAL_ERROR "libmoq not found. Please build libmoq first.")
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MOQ_INCLUDE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${MOQ_LIBRARY})

# Find VA-API
find_package(PkgConfig REQUIRED)
pkg_check_modules(VAAPI libva libva-drm)
if(VAAPI_FOUND)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${VAAPI_INCLUDE_DIRS})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${VAAPI_LIBRARIES})
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_VAAPI=1)
endif()

# Find FFmpeg
pkg_check_modules(FFMPEG libavcodec libavutil libswscale libswresample)
if(FFMPEG_FOUND)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_FFMPEG=1)
else()
    message(FATAL_ERROR "FFmpeg libraries not found")
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.c
    src/hang-source.c
    src/hang-source.h
    src/vaapi-decoder.c
    src/vaapi-decoder.h
    src/audio-decoder.c
    src/audio-decoder.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
