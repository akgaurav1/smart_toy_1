# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/gaurav/esp/esp-idf/components/bootloader/subproject"
  "/home/gaurav/esp/korvo_test_audio/build/bootloader"
  "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix"
  "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix/tmp"
  "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix/src/bootloader-stamp"
  "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix/src"
  "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/gaurav/esp/korvo_test_audio/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
