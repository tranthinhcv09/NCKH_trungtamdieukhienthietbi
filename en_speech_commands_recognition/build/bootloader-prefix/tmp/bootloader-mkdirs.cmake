# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "D:/Espressif/frameworks/esp-idf-v4.4.8/components/bootloader/subproject"
  "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader"
  "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader-prefix"
  "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader-prefix/tmp"
  "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader-prefix/src/bootloader-stamp"
  "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader-prefix/src"
  "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/Espressif/esp-skainet/examples/en_speech_commands_recognition/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
