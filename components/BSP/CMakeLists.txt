#指定源码的目录
set(src_dirs
    LED
    KEY
    IIC
    XL9555
    QMA6100P
    AP3216C
    BUZZER
    AT24C02)
set(include_dirs
    LED
    KEY
    IIC
    XL9555
    QMA6100P
    AP3216C
    BUZZER
    AT24C02)
#指定依赖的组件
set(requires
    esp_common
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_timer
    freertos
    log)
#设置源文件
idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires})
