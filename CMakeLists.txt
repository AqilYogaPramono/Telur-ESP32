cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_camera_mic_spk_manual_mode)

if(NOT "${IDF_TARGET}" STREQUAL "esp32s2" AND NOT "${IDF_TARGET}" STREQUAL "esp32s3")
    message(FATAL_ERROR
        "Target must be esp32s2 or esp32s3 (USB UVC / espressif/usb_stream). "
        "Current IDF_TARGET=${IDF_TARGET}. Run:\n"
        "  idf.py set-target esp32s3\n"
        "  idf.py fullclean build")
endif()
