cmake_minimum_required(VERSION 3.16)
project(obs-plugins)

# Each subdirectory under plugins/ is an independent OBS Studio plugin module.
add_subdirectory(plugins/obs-datetime-source)
add_subdirectory(plugins/obs-ltc-source)
