idf_component_register(SRCS "main.c" "epaper.c" "sc18is602.c"
                            "font8.c" "font12.c" "font16.c" "font24.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer)
