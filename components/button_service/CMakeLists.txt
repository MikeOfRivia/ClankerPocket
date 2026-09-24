idf_component_register(
    SRCS "button_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board button esp_driver_gpio
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
