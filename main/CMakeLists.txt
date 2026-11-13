set(srcs "main.c" "render.c" "led_out.c" "gamma.c" "wire.c" "astro.c" "audio.c" "sky.c" "progs.c" "automation.c" "firmware.c" "cmd.c" "console.c" "settings.c" "io.c")

# WiFi/NTP is optional (menuconfig: KTSN LED -> WiFi). CONFIG_ values aren't known yet when IDF resolves REQUIRES,
# so the network components are always listed; without net.c nothing references them and the linker drops them.
if(CONFIG_KTSN_WIFI)
    list(APPEND srcs "net.c")
endif()
if(CONFIG_KTSN_NET_CLI)
    list(APPEND srcs "netcli.c")
endif()
if(CONFIG_KTSN_OUTPUT_PARLIO)
    list(APPEND srcs "ws_parallel.c")
endif()
if(CONFIG_KTSN_HTTP_API)
    list(APPEND srcs "http_api.c")
endif()
if(CONFIG_KTSN_HTTP_API OR CONFIG_KTSN_SERIAL_API)
    list(APPEND srcs "api.c")
endif()
if(CONFIG_KTSN_OTA)
    list(APPEND srcs "ota.c")
endif()
if(CONFIG_KTSN_SERIAL_API)
    list(APPEND srcs "serial_api.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES dsl console esp_timer esp_driver_rmt esp_driver_parlio esp_driver_gpio esp_driver_ledc esp_adc esp_driver_usb_serial_jtag led_strip nvs_flash app_update
                                esp_wifi esp_netif esp_event lwip esp_http_server espressif__cjson espressif__mdns)

# The serial REPL's raw line, for program source with "strings" (cmd.c: __wrap_esp_console_split_argv)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_console_split_argv")
