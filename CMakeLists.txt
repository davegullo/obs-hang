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

option(MOQ_LOCAL "Path to moq repo for local development" "")

if(MOQ_LOCAL)
  add_subdirectory(${MOQ_LOCAL}/rs/libmoq moq)
else()
  if(NOT MOQ_VERSION)
    message(FATAL_ERROR "MOQ_LOCAL must be set or MOQ_VERSION must be defined for FetchContent builds")
  endif()

  include(FetchContent)
  FetchContent_Declare(
    moq
    URL
      https://github.com/kixelated/moq/releases/download/v${MOQ_VERSION}/moq-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}.tar.gz
  )
  FetchContent_MakeAvailable(moq)

  add_library(moq SHARED IMPORTED GLOBAL)
  set_target_properties(
    moq
    PROPERTIES
      IMPORTED_LOCATION "${moq_SOURCE_DIR}/lib/libmoq${CMAKE_SHARED_LIBRARY_SUFFIX}"
      INTERFACE_INCLUDE_DIRECTORIES "${moq_SOURCE_DIR}/include"
  )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE moq)

# NVDEC support is enabled if FFmpeg has CUDA support
# CUDA is only available on Linux/Windows, not macOS
if(NOT APPLE)
  # Check if CUDA is available
  find_library(CUDA_LIBRARY cuda PATHS /usr/local/cuda/lib64 /usr/local/cuda/lib)
  if(CUDA_LIBRARY OR EXISTS "/usr/local/cuda/include/cuda.h")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_NVDEC=1)
    message(STATUS "NVDEC/CUDA support enabled")
  else()
    message(STATUS "NVDEC/CUDA support disabled (CUDA not found)")
  endif()
else()
  message(STATUS "NVDEC/CUDA support disabled (not available on macOS)")
endif()

# Find FFmpeg
include(FindPkgConfig)
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
    src/nvdec-decoder.c
    src/nvdec-decoder.h
    src/audio-decoder.c
    src/audio-decoder.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
