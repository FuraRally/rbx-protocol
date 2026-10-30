idf_component_register(
    SRCS
        "src/rbx_frame.c"
        "src/rbx_pilot_state.c"
        "src/rbx_net.c"
        "src/rbx_box_server.c"
        "src/rbx_box_client.c"
        "src/rbx_pilot_client.c"
        "src/rbx_pilot_server.c"
        "src/rbx_link.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lwip freertos
    PRIV_REQUIRES esp_timer log
)
