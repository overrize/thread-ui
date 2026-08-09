idf_component_register(
    SRCS "app_main.c" "display.c" "thread_anim.c" "input_wheel.c" "ui_widgets.c" "menu.c" "knob_uart.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer esp_lcd_sh8501 esp_driver_pcnt esp_driver_gpio esp_driver_uart
)
