idf_component_register(
    SRCS "main.c"
         "wb_app.c"
         "wb_model.c"
         "wb_parse.c"
         "wb_portal.c"
         "wb_store.c"
         "wb_ui.c"
         "../assets/fonts/wb_font_cjk_14.c"
         "../assets/fonts/wb_font_cjk_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_http_server esp_netif esp_timer esp_wifi nvs_flash
)
