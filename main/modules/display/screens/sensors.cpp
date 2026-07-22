/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "sensors.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "modules/sensor_context.hpp"
#include "private/utils.hpp"

using namespace esp_brookesia;

namespace {

constexpr int SCREEN_WIDTH = 240;
constexpr int SCREEN_HEIGHT = 284;
constexpr int CARD_WIDTH = 108;
constexpr int CARD_HEIGHT = 76;
constexpr int SAMPLE_WHILE_VISIBLE_MS = 5000;

lv_color_t status_color(SensorContext::Status status)
{
    switch (status) {
    case SensorContext::Status::Ready:
        return lv_color_hex(0x53D769);
    case SensorContext::Status::Waiting:
        return lv_color_hex(0xF5C451);
    case SensorContext::Status::Stopped:
        return lv_color_hex(0x777B84);
    default:
        return lv_color_hex(0xFF5C5C);
    }
}

const char *status_text(SensorContext::Status status)
{
    switch (status) {
    case SensorContext::Status::Ready:
        return "READY";
    case SensorContext::Status::Waiting:
        return "WAITING";
    case SensorContext::Status::NotDetected:
        return "NO SENSOR";
    case SensorContext::Status::BusUnavailable:
        return "I2C ERROR";
    case SensorContext::Status::InitFailed:
        return "INIT ERROR";
    case SensorContext::Status::ReadFailed:
        return "READ ERROR";
    case SensorContext::Status::Stopped:
    default:
        return "STOPPED";
    }
}

std::string age_text(int64_t age_ms)
{
    char text[24] = {};
    if (age_ms < 1000) {
        return "NOW";
    }
    if (age_ms < 60000) {
        std::snprintf(text, sizeof(text), "%llds", static_cast<long long>(age_ms / 1000));
    } else {
        std::snprintf(text, sizeof(text), "%lldm", static_cast<long long>(age_ms / 60000));
    }
    return text;
}

std::string upper_voc(std::string trend)
{
    std::transform(trend.begin(), trend.end(), trend.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return "VOC " + trend;
}

} // namespace

ScreenSensors::ScreenSensors():
    StateBase(BROOKESIA_DESCRIBE_TO_STR(DisplayScreen::Sensors))
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    init_screen_ = lv_screen_active();
    BROOKESIA_CHECK_NULL_EXIT(init_screen_, "Failed to get active screen");
}

ScreenSensors::~ScreenSensors()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    if (refresh_timer_ != nullptr) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
}

ScreenSensors::Card ScreenSensors::create_card(int x, int y, const char *title, const char *unit)
{
    Card card;
    card.container = lv_obj_create(screen_);
    lv_obj_set_pos(card.container, x, y);
    lv_obj_set_size(card.container, CARD_WIDTH, CARD_HEIGHT);
    lv_obj_remove_flag(card.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card.container, lv_color_hex(0x111318), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card.container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card.container, lv_color_hex(0x292D35), LV_PART_MAIN);
    lv_obj_set_style_border_width(card.container, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card.container, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card.container, 0, LV_PART_MAIN);

    auto *title_label = lv_label_create(card.container);
    lv_label_set_text(title_label, title);
    lv_obj_set_pos(title_label, 10, 7);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x9297A1), LV_PART_MAIN);

    card.value = lv_label_create(card.container);
    lv_label_set_text(card.value, "--");
    lv_obj_set_pos(card.value, 9, 27);
    lv_obj_set_width(card.value, 75);
    lv_obj_set_style_text_font(card.value, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(card.value, lv_color_white(), LV_PART_MAIN);

    card.unit = lv_label_create(card.container);
    lv_label_set_text(card.unit, unit);
    lv_obj_align(card.unit, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_text_color(card.unit, lv_color_hex(0x9297A1), LV_PART_MAIN);
    return card;
}

void ScreenSensors::create_ui()
{
    if (screen_ != nullptr) {
        return;
    }

    screen_ = lv_obj_create(nullptr);
    BROOKESIA_CHECK_NULL_EXIT(screen_, "Failed to create sensor screen");
    lv_obj_set_size(screen_, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_, 0, LV_PART_MAIN);

    auto *title = lv_label_create(screen_);
    lv_label_set_text(title, "ENVIRONMENT");
    lv_obj_set_pos(title, 10, 11);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);

    status_dot_ = lv_obj_create(screen_);
    lv_obj_set_pos(status_dot_, 105, 14);
    lv_obj_set_size(status_dot_, 8, 8);
    lv_obj_remove_flag(status_dot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_dot_, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, LV_PART_MAIN);

    status_label_ = lv_label_create(screen_);
    lv_label_set_text(status_label_, "WAITING");
    lv_obj_set_pos(status_label_, 118, 10);
    lv_obj_set_width(status_label_, 80);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xBFC3CB), LV_PART_MAIN);

    auto *retry_button = lv_button_create(screen_);
    lv_obj_set_pos(retry_button, 201, 4);
    lv_obj_set_size(retry_button, 32, 29);
    lv_obj_set_style_bg_color(retry_button, lv_color_hex(0x1C2027), LV_PART_MAIN);
    lv_obj_set_style_radius(retry_button, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(retry_button, retry_button_callback, LV_EVENT_CLICKED, this);
    auto *retry_label = lv_label_create(retry_button);
    lv_label_set_text(retry_label, LV_SYMBOL_REFRESH);
    lv_obj_center(retry_label);

    temperature_card_ = create_card(8, 40, "TEMP", "C");
    humidity_card_ = create_card(124, 40, "HUM", "%");
    pressure_card_ = create_card(8, 124, "PRESS", "hPa");
    gas_card_ = create_card(124, 124, "GAS", "kOhm");

    auto *footer = lv_obj_create(screen_);
    lv_obj_set_pos(footer, 8, 208);
    lv_obj_set_size(footer, 224, 68);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x0B0D11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(footer, lv_color_hex(0x292D35), LV_PART_MAIN);
    lv_obj_set_style_border_width(footer, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(footer, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_all(footer, 0, LV_PART_MAIN);

    voc_label_ = lv_label_create(footer);
    lv_label_set_text(voc_label_, "VOC UNKNOWN");
    lv_obj_set_pos(voc_label_, 10, 7);
    lv_obj_set_style_text_color(voc_label_, lv_color_white(), LV_PART_MAIN);

    age_label_ = lv_label_create(footer);
    lv_label_set_text(age_label_, "--");
    lv_obj_align(age_label_, LV_ALIGN_TOP_RIGHT, -10, 7);
    lv_obj_set_style_text_color(age_label_, lv_color_hex(0x9297A1), LV_PART_MAIN);

    detail_label_ = lv_label_create(footer);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(detail_label_, 10, 28);
    lv_obj_set_size(detail_label_, 204, 34);
    lv_obj_set_style_text_color(detail_label_, lv_color_hex(0x9297A1), LV_PART_MAIN);
    lv_label_set_text(detail_label_, "Waiting for BME690...");
}

bool ScreenSensors::on_enter(const std::string &from_state, const std::string &action)
{
    BROOKESIA_LOGI("Entering '%1%' from '%2%' with action '%3%'", get_name(), from_state, action);
    create_ui();
    BROOKESIA_CHECK_NULL_RETURN(screen_, false, "Failed to create sensor screen");
    lv_screen_load(screen_);

    auto *display = lv_display_get_default();
    BROOKESIA_CHECK_NULL_RETURN(display, false, "Failed to get default display");
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        esp_lv_adapter_set_dummy_draw(display, false), false, "Failed to enable LVGL sensor drawing"
    );

    SensorContext::get_instance().request_sample();
    last_sample_request_ms_ = esp_timer_get_time() / 1000;
    refresh_ui(true);
    if (refresh_timer_ == nullptr) {
        refresh_timer_ = lv_timer_create(refresh_timer_callback, 1000, this);
    }
    return true;
}

bool ScreenSensors::on_exit(const std::string &to_state, const std::string &action)
{
    BROOKESIA_LOGI("Exiting '%1%' to '%2%' with action '%3%'", get_name(), to_state, action);
    if (refresh_timer_ != nullptr) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    lv_screen_load(init_screen_);
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    status_dot_ = nullptr;
    status_label_ = nullptr;
    voc_label_ = nullptr;
    age_label_ = nullptr;
    detail_label_ = nullptr;
    temperature_card_ = {};
    humidity_card_ = {};
    pressure_card_ = {};
    gas_card_ = {};
    label_cache_.fill({});
    return true;
}

void ScreenSensors::update_label(lv_obj_t *label, std::string &cache, std::string value, bool force)
{
    if (force || cache != value) {
        cache = std::move(value);
        lv_label_set_text(label, cache.c_str());
    }
}

void ScreenSensors::refresh_ui(bool force)
{
    auto &sensor = SensorContext::get_instance();
    const int64_t now = esp_timer_get_time() / 1000;
    if (now - last_sample_request_ms_ >= SAMPLE_WHILE_VISIBLE_MS) {
        sensor.request_sample();
        last_sample_request_ms_ = now;
    }

    const auto data = sensor.get_view_data();
    lv_obj_set_style_bg_color(status_dot_, status_color(data.status), LV_PART_MAIN);
    update_label(status_label_, label_cache_[0], status_text(data.status), force);

    char buffer[64] = {};
    if (data.has_sample) {
        std::snprintf(buffer, sizeof(buffer), "%.1f", data.temperature_c);
        update_label(temperature_card_.value, label_cache_[1], buffer, force);
        std::snprintf(buffer, sizeof(buffer), "%.0f", data.humidity_percent);
        update_label(humidity_card_.value, label_cache_[2], buffer, force);
        std::snprintf(buffer, sizeof(buffer), "%.0f", data.pressure_hpa);
        update_label(pressure_card_.value, label_cache_[3], buffer, force);
        if (data.gas_valid) {
            std::snprintf(buffer, sizeof(buffer), "%.0f", data.gas_resistance_ohm / 1000.0f);
        } else {
            std::snprintf(buffer, sizeof(buffer), "--");
        }
        update_label(gas_card_.value, label_cache_[4], buffer, force);
        update_label(voc_label_, label_cache_[5], upper_voc(data.voc_trend), force);
        update_label(age_label_, label_cache_[6], age_text(data.updated_ms_ago), force);
    } else {
        update_label(temperature_card_.value, label_cache_[1], "--", force);
        update_label(humidity_card_.value, label_cache_[2], "--", force);
        update_label(pressure_card_.value, label_cache_[3], "--", force);
        update_label(gas_card_.value, label_cache_[4], "--", force);
        update_label(voc_label_, label_cache_[5], "VOC UNKNOWN", force);
        update_label(age_label_, label_cache_[6], "--", force);
    }

    std::string detail;
    if (data.status == SensorContext::Status::Ready) {
        std::snprintf(
            buffer, sizeof(buffer), "BME690 @0x%02X  |  %u samples",
            data.i2c_address, static_cast<unsigned>(data.sample_count)
        );
        detail = buffer;
    } else if (data.has_sample) {
        detail = "Showing last sample. " + data.reason;
    } else if (data.status == SensorContext::Status::NotDetected) {
        detail = "Power off, insert BME690, then reboot.";
    } else {
        detail = data.reason;
    }
    update_label(detail_label_, label_cache_[7], std::move(detail), force);
}

void ScreenSensors::refresh_timer_callback(lv_timer_t *timer)
{
    auto *screen = static_cast<ScreenSensors *>(lv_timer_get_user_data(timer));
    if (screen != nullptr) {
        screen->refresh_ui();
    }
}

void ScreenSensors::retry_button_callback(lv_event_t *event)
{
    auto *screen = static_cast<ScreenSensors *>(lv_event_get_user_data(event));
    if (screen == nullptr) {
        return;
    }
    SensorContext::get_instance().request_sample();
    screen->last_sample_request_ms_ = esp_timer_get_time() / 1000;
    screen->refresh_ui(true);
}
