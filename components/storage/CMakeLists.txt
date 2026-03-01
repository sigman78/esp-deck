include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         storage.c
    SRCS_DEV     storage_dev.c
    SRCS_SIM     storage_sim.c
    INCLUDE_DIRS include
    REQUIRES_DEV esp_littlefs
)

if(NOT BUILD_SIMULATOR)
littlefs_create_partition_image(storage ../../sim_storage FLASH_IN_PROJECT)
endif()