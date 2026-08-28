cmake_minimum_required(VERSION 3.16)

# Credentials (Wi-Fi SSID/password, Yahoo Client ID) belong in
# sdkconfig.defaults.local, which .gitignore excludes. See README.md.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(laundry-weather)
