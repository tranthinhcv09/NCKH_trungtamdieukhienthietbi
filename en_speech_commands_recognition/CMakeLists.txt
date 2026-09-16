cmake_minimum_required(VERSION 3.10)

set(EXTRA_COMPONENT_DIRS
    ../../components
    )

set(ENV{IDF_PYTHON_ENV_PATH} "D:/Espressif/v5.3/python_env/idf4.4_py3.10_env")
set(ENV{PATH} "D:/Espressif/v5.3/python_env/idf4.4_py3.10_env/Scripts;D:/Espressif/tools/xtensa-esp32s3-elf/esp-2021r2-patch5-8.4.0/xtensa-esp32s3-elf/bin;$ENV{PATH}")
set(ENV{CC} "D:/Espressif/tools/xtensa-esp32s3-elf/esp-2021r2-patch5-8.4.0/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-gcc.exe")
set(ENV{CXX} "D:/Espressif/tools/xtensa-esp32s3-elf/esp-2021r2-patch5-8.4.0/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-g++.exe")
set(ENV{ASM} "D:/Espressif/tools/xtensa-esp32s3-elf/esp-2021r2-patch5-8.4.0/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-gcc.exe")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(speech_commands_recognition)
