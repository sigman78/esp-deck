include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS_DEV     wifi_manager.c
    SRCS_SIM     wifi_manager_sim.c
    INCLUDE_DIRS include
    REQUIRES_DEV esp_wifi esp_netif nvs_flash
)
