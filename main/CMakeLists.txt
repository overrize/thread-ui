idf_component_register(
    SRCS "app_main.c" "display.c" "thread_anim.c" "input_wheel.c" "ui_widgets.c" "menu.c" "knob_uart.c"
         "qd_app.c" "qd/ui_render.c" "qd/demo_state.c"
    INCLUDE_DIRS "." "qd"
    REQUIRES esp_lcd driver esp_timer esp_lcd_sh8501 esp_driver_pcnt esp_driver_gpio esp_driver_uart
)
# 新 UI 渲染热路径：-O2 之外再开循环展开（与 QuantDriver 点亮包一致）
set_source_files_properties(qd/ui_render.c PROPERTIES COMPILE_OPTIONS "-O2;-funroll-loops")
