#
# Copyright 2021 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

cmake_minimum_required(VERSION 3.7.1 FATAL_ERROR)
set(CMAKE_CXX_STANDARD 11)

project(libdav1dJNI C CXX)

# Devices using armeabi-v7a are not required to support
# Neon which is why Neon is disabled by default for
# armeabi-v7a build. This flag enables it.
if(${ANDROID_ABI} MATCHES "armeabi-v7a")
    add_compile_options("-mfpu=neon")
    add_compile_options("-marm")
    add_compile_options("-fPIC")
endif()

string(TOLOWER "${CMAKE_BUILD_TYPE}" build_type)
if(build_type MATCHES "^rel")
    add_compile_options("-O2")
endif()

set(libav1d_jni_root "${CMAKE_CURRENT_SOURCE_DIR}")

include_directories(jni/include)

add_library(
        dav1d
        SHARED
        IMPORTED
)

set_target_properties(
        dav1d
             PROPERTIES IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/${ANDROID_ABI}/libdav1d.so
)

file(GLOB_RECURSE C_SRC_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cc
        )
# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds it for you.
# Gradle automatically packages shared libraries with your APK.

add_library( # Sets the name of the library.
             dav1d_jni

             # Sets the library as a shared library.
             SHARED

             # Provides a relative path to your source file(s).
             # Associated headers in the same location as their source
             # file are automatically included.
             ${SRC_LIST}
             ${C_SRC_LIST})

# Locate NDK log library.
find_library(android_log_lib log)

target_link_libraries( # Specifies the target library.
                       dav1d_jni
                       android
                       dav1d
                       ${android_log_lib})

