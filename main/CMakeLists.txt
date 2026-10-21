idf_component_register(
    SRCS "main.c"
         "wb_adpcm.c"
         "wb_app.c"
         "wb_audio.c"
         "wb_model.c"
         "wb_net.c"
         "wb_parse.c"
         "wb_portal.c"
         "wb_slots.c"
         "wb_store.c"
         "wb_sync.c"
         "wb_ui.c"
         "../assets/fonts/wb_font_cjk_14.c"
         "../assets/fonts/wb_font_cjk_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_hw_support esp_netif
             esp_partition esp_timer esp_wifi mbedtls nvs_flash
)
