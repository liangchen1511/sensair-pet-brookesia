/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "lvgl.h"
#include "brookesia/lib_utils/state_base.hpp"
#include "common.hpp"

class ScreenSensors : public esp_brookesia::lib_utils::StateBase {
public:
    ScreenSensors();
    ~ScreenSensors();

    bool on_enter(const std::string &from_state, const std::string &action) override;
    bool on_exit(const std::string &to_state, const std::string &action) override;

private:
    struct Card {
        lv_obj_t *container = nullptr;
        lv_obj_t *value = nullptr;
        lv_obj_t *unit = nullptr;
    };

    Card create_card(int x, int y, const char *title, const char *unit);
    void create_ui();
    void refresh_ui(bool force = false);
    void update_label(lv_obj_t *label, std::string &cache, std::string value, bool force);

    static void refresh_timer_callback(lv_timer_t *timer);
    static void retry_button_callback(lv_event_t *event);

    lv_obj_t *init_screen_ = nullptr;
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *status_dot_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *voc_label_ = nullptr;
    lv_obj_t *age_label_ = nullptr;
    lv_obj_t *detail_label_ = nullptr;
    lv_timer_t *refresh_timer_ = nullptr;
    Card temperature_card_{};
    Card humidity_card_{};
    Card pressure_card_{};
    Card gas_card_{};
    int64_t last_sample_request_ms_ = 0;
    std::array<std::string, 8> label_cache_{};
};
