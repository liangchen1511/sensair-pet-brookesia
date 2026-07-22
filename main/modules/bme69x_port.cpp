/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "bme69x_port.hpp"

#include <array>
#include <cstring>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "BME69xPort";
constexpr int I2C_TIMEOUT_MS = 80;
constexpr uint32_t I2C_SPEED_HZ = 100000;
constexpr size_t MAX_WRITE_BYTES = 32;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_device = nullptr;
uint8_t s_address = 0;
esp_err_t s_last_error = ESP_ERR_INVALID_STATE;

int8_t i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *)
{
    if (s_device == nullptr || reg_data == nullptr || len == 0) {
        s_last_error = ESP_ERR_INVALID_STATE;
        return BME69X_E_COM_FAIL;
    }

    s_last_error = i2c_master_transmit_receive(
                       s_device, &reg_addr, 1, reg_data, len, I2C_TIMEOUT_MS
                   );
    if (s_last_error != ESP_OK) {
        ESP_LOGW(TAG, "Read 0x%02x failed: %s", reg_addr, esp_err_to_name(s_last_error));
        return BME69X_E_COM_FAIL;
    }
    return BME69X_OK;
}

int8_t i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *)
{
    if (s_device == nullptr || reg_data == nullptr || len == 0 || len + 1 > MAX_WRITE_BYTES) {
        s_last_error = ESP_ERR_INVALID_ARG;
        return BME69X_E_COM_FAIL;
    }

    std::array<uint8_t, MAX_WRITE_BYTES> buffer{};
    buffer[0] = reg_addr;
    std::memcpy(buffer.data() + 1, reg_data, len);
    s_last_error = i2c_master_transmit(s_device, buffer.data(), len + 1, I2C_TIMEOUT_MS);
    if (s_last_error != ESP_OK) {
        ESP_LOGW(TAG, "Write 0x%02x failed: %s", reg_addr, esp_err_to_name(s_last_error));
        return BME69X_E_COM_FAIL;
    }
    return BME69X_OK;
}

void delay_us(uint32_t period, void *)
{
    if (period < 1000) {
        esp_rom_delay_us(period);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period + 999) / 1000));
    }
}

} // namespace

esp_err_t bme69x_port_init(
    bme69x_dev *device,
    i2c_master_bus_handle_t bus_handle,
    uint8_t *detected_address
)
{
    if (device == nullptr || bus_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    bme69x_port_deinit();
    s_bus = bus_handle;

    constexpr std::array<uint8_t, 2> addresses{
        BME69X_I2C_ADDR_LOW,
        BME69X_I2C_ADDR_HIGH,
    };

    for (const auto address : addresses) {
        const auto probe_result = i2c_master_probe(s_bus, address, I2C_TIMEOUT_MS);
        ESP_LOGI(TAG, "Probe 0x%02x: %s", address, esp_err_to_name(probe_result));
        if (probe_result != ESP_OK) {
            s_last_error = probe_result;
            continue;
        }

        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = I2C_SPEED_HZ,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = false,
            },
        };
        s_last_error = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
        if (s_last_error != ESP_OK) {
            ESP_LOGW(TAG, "Add device 0x%02x failed: %s", address, esp_err_to_name(s_last_error));
            s_device = nullptr;
            continue;
        }

        s_address = address;
        uint8_t chip_id = 0;
        if (i2c_read(BME69X_REG_CHIP_ID, &chip_id, 1, nullptr) == BME69X_OK &&
            chip_id == BME69X_CHIP_ID) {
            device->read = i2c_read;
            device->write = i2c_write;
            device->delay_us = delay_us;
            device->intf = BME69X_I2C_INTF;
            device->intf_ptr = &s_address;
            device->amb_temp = 25;
            if (detected_address != nullptr) {
                *detected_address = s_address;
            }
            s_last_error = ESP_OK;
            ESP_LOGI(TAG, "Detected BME69x at 0x%02x, chip id 0x%02x", s_address, chip_id);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Unexpected chip id 0x%02x at 0x%02x", chip_id, address);
        i2c_master_bus_rm_device(s_device);
        s_device = nullptr;
        s_address = 0;
        s_last_error = ESP_ERR_NOT_FOUND;
    }

    s_bus = nullptr;
    return ESP_ERR_NOT_FOUND;
}

void bme69x_port_deinit()
{
    if (s_device != nullptr) {
        const auto result = i2c_master_bus_rm_device(s_device);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Remove device failed: %s", esp_err_to_name(result));
        }
    }
    s_device = nullptr;
    s_bus = nullptr;
    s_address = 0;
}

esp_err_t bme69x_port_get_last_error()
{
    return s_last_error;
}

