/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_err.h"

extern "C" {
#include "bme69x.h"
}

/**
 * Attach one persistent BME69x device to an already initialized Board Manager
 * I2C bus. The two legal addresses are probed before a device handle is added,
 * so a missing sub-board is reported as ESP_ERR_NOT_FOUND instead of the less
 * useful ESP_ERR_INVALID_STATE returned by a checked transaction on NACK.
 */
esp_err_t bme69x_port_init(
    bme69x_dev *device,
    i2c_master_bus_handle_t bus_handle,
    uint8_t *detected_address
);

void bme69x_port_deinit();
esp_err_t bme69x_port_get_last_error();

