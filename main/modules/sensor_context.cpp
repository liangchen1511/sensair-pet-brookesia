/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "sensor_context.hpp"

#include <cmath>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "brookesia/service_custom/service_custom.hpp"

extern "C" {
#include "bme69x.h"
#include "bme69x_defs.h"
#include "common.h"
}

using namespace esp_brookesia;

namespace {
constexpr const char *TAG = "SensorContext";
constexpr gpio_num_t BME690_SDO_PIN = GPIO_NUM_9;
constexpr i2c_port_t I2C_MASTER_NUM = I2C_NUM_0;
constexpr uint32_t SAMPLE_INTERVAL_MS = 30000;
constexpr uint32_t FIRST_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t SENSOR_TASK_STACK = 5 * 1024;
constexpr UBaseType_t SENSOR_TASK_PRIORITY = 1;

constexpr const char *FN_GET_ENVIRONMENT_SNAPSHOT = "GetEnvironmentSnapshot";
constexpr const char *FN_GET_ENVIRONMENT_TREND = "GetEnvironmentTrend";
constexpr const char *FN_GET_COMFORT_STATUS = "GetComfortStatus";

struct bme69x_dev s_bme;

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

const char *trend_from_delta(float delta, float threshold)
{
    if (delta > threshold) {
        return "rising";
    }
    if (delta < -threshold) {
        return "falling";
    }
    return "stable";
}

boost::json::object make_error_object(const std::string &reason)
{
    return {
        {"available", false},
        {"reason", reason},
    };
}

service::FunctionResult ok_json(boost::json::object object)
{
    return {
        .success = true,
        .data = service::FunctionValue(std::move(object)),
    };
}
} // namespace

bool SensorContext::start(std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler)
{
    if (running_.load()) {
        return true;
    }

    if (task_scheduler == nullptr) {
        unavailable_reason_ = "task scheduler is null";
        return false;
    }
    task_scheduler_ = std::move(task_scheduler);

    if (!register_service_functions()) {
        BROOKESIA_LOGW("Failed to register environment service functions");
    }

    running_.store(true);
    auto ret = xTaskCreate(
        sample_task_entry,
        "SensorContext",
        SENSOR_TASK_STACK,
        this,
        SENSOR_TASK_PRIORITY,
        &sample_task_handle_
    );
    if (ret != pdPASS) {
        running_.store(false);
        sample_task_handle_ = nullptr;
        unavailable_reason_ = "failed to create sensor task";
        BROOKESIA_LOGE("Failed to create sensor task");
        return false;
    }

    return true;
}

void SensorContext::stop()
{
    running_.store(false);
}

std::vector<std::string> SensorContext::get_mcp_function_names()
{
    return {
        FN_GET_ENVIRONMENT_SNAPSHOT,
        FN_GET_ENVIRONMENT_TREND,
        FN_GET_COMFORT_STATUS,
    };
}

bool SensorContext::register_service_functions()
{
    auto &service_manager = service::ServiceManager::get_instance();
    auto binding = service_manager.bind(service::CustomServiceName);
    if (!binding.is_valid()) {
        BROOKESIA_LOGE("Failed to bind CustomService");
        return false;
    }
    service_bindings_.push_back(std::move(binding));

    auto &custom_service = service::CustomService::get_instance();

    bool ok = true;
    ok &= custom_service.register_function(
        {
            .name = FN_GET_ENVIRONMENT_SNAPSHOT,
            .description =
                "Get local BME690 environment snapshot. Returns JSON with temperature_c, humidity_percent, "
                "pressure_hpa, gas_resistance_ohm, voc_trend and data age. Use this when the user asks current "
                "room temperature, humidity, air pressure, gas resistance, or whether the room feels stuffy.",
            .parameters = {},
            .require_scheduler = false,
        },
        [](const service::FunctionParameterMap &) -> service::FunctionResult {
            return ok_json(SensorContext::get_instance().get_snapshot_json());
        }
    );

    ok &= custom_service.register_function(
        {
            .name = FN_GET_ENVIRONMENT_TREND,
            .description =
                "Get local BME690 short-term trend. Returns JSON trend for temperature, humidity, pressure, "
                "gas resistance and inferred VOC trend. VOC trend is inferred from gas resistance direction, "
                "not an absolute ppm value.",
            .parameters = {},
            .require_scheduler = false,
        },
        [](const service::FunctionParameterMap &) -> service::FunctionResult {
            return ok_json(SensorContext::get_instance().get_trend_json());
        }
    );

    ok &= custom_service.register_function(
        {
            .name = FN_GET_COMFORT_STATUS,
            .description =
                "Get a human-friendly local comfort status from temperature, humidity and gas trend. "
                "Returns JSON with comfort level and suggested natural-language explanation.",
            .parameters = {},
            .require_scheduler = false,
        },
        [](const service::FunctionParameterMap &) -> service::FunctionResult {
            return ok_json(SensorContext::get_instance().get_comfort_json());
        }
    );

    return ok;
}

bool SensorContext::init_bme690()
{
    gpio_config_t sdo_conf = {};
    sdo_conf.pin_bit_mask = (1ULL << BME690_SDO_PIN);
    sdo_conf.mode = GPIO_MODE_OUTPUT;
    sdo_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    sdo_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    sdo_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&sdo_conf));
    gpio_set_level(BME690_SDO_PIN, 0);

    i2c_master_bus_handle_t i2c_bus = nullptr;
    esp_err_t ret = i2c_master_get_bus_handle(I2C_MASTER_NUM, &i2c_bus);
    if (ret != ESP_OK || i2c_bus == nullptr) {
        unavailable_reason_ = std::string("i2c bus handle unavailable: ") + esp_err_to_name(ret);
        return false;
    }

    bme69x_set_i2c_bus_handle(i2c_bus);

    int8_t rslt = bme69x_interface_init(&s_bme, BME69X_I2C_INTF);
    if (rslt != BME69X_OK) {
        unavailable_reason_ = "BME690 interface init failed";
        return false;
    }

    rslt = bme69x_init(&s_bme);
    if (rslt != BME69X_OK) {
        unavailable_reason_ = "BME690 sensor init failed";
        return false;
    }

    bme69x_conf conf = {};
    conf.filter = BME69X_FILTER_OFF;
    conf.odr = BME69X_ODR_NONE;
    conf.os_hum = BME69X_OS_4X;
    conf.os_pres = BME69X_OS_4X;
    conf.os_temp = BME69X_OS_4X;
    rslt = bme69x_set_conf(&conf, &s_bme);
    if (rslt != BME69X_OK) {
        unavailable_reason_ = "BME690 set conf failed";
        return false;
    }

    bme69x_heatr_conf heatr_conf = {};
    heatr_conf.enable = BME69X_ENABLE;
    heatr_conf.heatr_temp = 300;
    heatr_conf.heatr_dur = 100;
    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &s_bme);
    if (rslt != BME69X_OK) {
        unavailable_reason_ = "BME690 heater conf failed";
        return false;
    }

    bme_available_.store(true);
    unavailable_reason_.clear();
    BROOKESIA_LOGI("BME690 initialized, chip id: 0x%02x", s_bme.chip_id);
    return true;
}

bool SensorContext::read_bme690_once(Sample &sample)
{
    bme69x_conf conf = {};
    conf.filter = BME69X_FILTER_OFF;
    conf.odr = BME69X_ODR_NONE;
    conf.os_hum = BME69X_OS_4X;
    conf.os_pres = BME69X_OS_4X;
    conf.os_temp = BME69X_OS_4X;

    bme69x_heatr_conf heatr_conf = {};
    heatr_conf.enable = BME69X_ENABLE;
    heatr_conf.heatr_temp = 300;
    heatr_conf.heatr_dur = 100;

    int8_t rslt = bme69x_set_conf(&conf, &s_bme);
    if (rslt != BME69X_OK) {
        return false;
    }
    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &s_bme);
    if (rslt != BME69X_OK) {
        return false;
    }
    rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &s_bme);
    if (rslt != BME69X_OK) {
        return false;
    }

    uint32_t delay_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &s_bme) + heatr_conf.heatr_dur * 1000;
    s_bme.delay_us(delay_us, s_bme.intf_ptr);

    bme69x_data data = {};
    uint8_t n_data = 0;
    rslt = bme69x_get_data(BME69X_FORCED_MODE, &data, &n_data, &s_bme);
    if (rslt != BME69X_OK || n_data == 0) {
        return false;
    }

    sample.timestamp_ms = now_ms();
    sample.temperature_c = data.temperature;
    sample.humidity_percent = data.humidity;
    sample.pressure_hpa = data.pressure / 100.0f;
    sample.gas_resistance_ohm = (data.status & BME69X_GASM_VALID_MSK) ? data.gas_resistance : 0.0f;
    sample.valid = true;
    return true;
}

void SensorContext::sample_task_entry(void *arg)
{
    static_cast<SensorContext *>(arg)->sample_task();
}

void SensorContext::sample_task()
{
    BROOKESIA_LOGI("Sensor context task started");

    while (running_.load()) {
        if (!bme_available_.load()) {
            if (!init_bme690()) {
                BROOKESIA_LOGW("BME690 unavailable: %s", unavailable_reason_.c_str());
                vTaskDelay(pdMS_TO_TICKS(FIRST_RETRY_INTERVAL_MS));
                continue;
            }
        }

        Sample sample;
        if (read_bme690_once(sample)) {
            push_sample(sample);
            BROOKESIA_LOGI(
                "BME690 sample: %.1f C, %.1f %%, %.1f hPa, gas %.0f ohm",
                sample.temperature_c,
                sample.humidity_percent,
                sample.pressure_hpa,
                sample.gas_resistance_ohm
            );
        } else {
            unavailable_reason_ = "BME690 sample failed";
            BROOKESIA_LOGW("%s", unavailable_reason_.c_str());
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }

    bme69x_coines_deinit();
    bme_available_.store(false);
    sample_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void SensorContext::push_sample(const Sample &sample)
{
    std::lock_guard lock(samples_mutex_);
    samples_[sample_write_index_] = sample;
    sample_write_index_ = (sample_write_index_ + 1) % samples_.size();
    if (sample_count_ < samples_.size()) {
        sample_count_++;
    }
}

std::vector<SensorContext::Sample> SensorContext::copy_samples_locked() const
{
    std::lock_guard lock(samples_mutex_);
    std::vector<Sample> out;
    out.reserve(sample_count_);
    for (size_t i = 0; i < sample_count_; ++i) {
        const size_t idx = (sample_write_index_ + samples_.size() - sample_count_ + i) % samples_.size();
        if (samples_[idx].valid) {
            out.push_back(samples_[idx]);
        }
    }
    return out;
}

boost::json::object SensorContext::get_snapshot_json() const
{
    auto samples = copy_samples_locked();
    if (samples.empty()) {
        return make_error_object(unavailable_reason_.empty() ? "waiting for first sample" : unavailable_reason_);
    }

    const auto &last = samples.back();
    auto trend = get_trend_json();
    return {
        {"available", true},
        {"temperature_c", last.temperature_c},
        {"humidity_percent", last.humidity_percent},
        {"pressure_hpa", last.pressure_hpa},
        {"gas_resistance_ohm", last.gas_resistance_ohm},
        {"voc_trend", trend.if_contains("voc_trend") ? trend.at("voc_trend") : boost::json::value("unknown")},
        {"updated_ms_ago", now_ms() - last.timestamp_ms},
        {"note", "VOC trend is inferred from BME690 gas resistance, not an absolute ppm value."},
    };
}

boost::json::object SensorContext::get_trend_json() const
{
    auto samples = copy_samples_locked();
    if (samples.size() < 2) {
        return make_error_object("not enough samples for trend");
    }

    const auto &first = samples.front();
    const auto &last = samples.back();
    const float temp_delta = last.temperature_c - first.temperature_c;
    const float humidity_delta = last.humidity_percent - first.humidity_percent;
    const float pressure_delta = last.pressure_hpa - first.pressure_hpa;
    const float gas_delta = last.gas_resistance_ohm - first.gas_resistance_ohm;
    const float gas_base = std::max(std::fabs(first.gas_resistance_ohm), 1.0f);
    const float gas_delta_percent = gas_delta * 100.0f / gas_base;

    const char *gas_resistance_trend = trend_from_delta(gas_delta_percent, 12.0f);
    const char *voc_trend = "stable";
    if (gas_delta_percent < -12.0f) {
        voc_trend = "rising";
    } else if (gas_delta_percent > 12.0f) {
        voc_trend = "falling";
    }

    return {
        {"available", true},
        {"window_seconds", (last.timestamp_ms - first.timestamp_ms) / 1000},
        {"sample_count", static_cast<int>(samples.size())},
        {"temperature_trend", trend_from_delta(temp_delta, 0.5f)},
        {"humidity_trend", trend_from_delta(humidity_delta, 3.0f)},
        {"pressure_trend", trend_from_delta(pressure_delta, 1.5f)},
        {"gas_resistance_trend", gas_resistance_trend},
        {"voc_trend", voc_trend},
        {"temperature_delta_c", temp_delta},
        {"humidity_delta_percent", humidity_delta},
        {"pressure_delta_hpa", pressure_delta},
        {"gas_resistance_delta_percent", gas_delta_percent},
        {"note", "When gas resistance falls, possible VOC/odor load is treated as rising."},
    };
}

boost::json::object SensorContext::get_comfort_json() const
{
    auto snapshot = get_snapshot_json();
    if (!snapshot.contains("available") || !snapshot.at("available").as_bool()) {
        return snapshot;
    }

    const float temp = static_cast<float>(snapshot.at("temperature_c").as_double());
    const float humidity = static_cast<float>(snapshot.at("humidity_percent").as_double());
    std::string level = "comfortable";
    std::string suggestion = "Current room environment looks comfortable.";

    if (temp >= 30.0f) {
        level = "hot";
        suggestion = "The room is a bit hot. Consider cooling or increasing airflow.";
    } else if (temp <= 16.0f) {
        level = "cold";
        suggestion = "The room is a bit cold. Consider warming up.";
    } else if (humidity >= 70.0f) {
        level = "humid";
        suggestion = "Humidity is high. Ventilation or dehumidifying may help.";
    } else if (humidity <= 35.0f) {
        level = "dry";
        suggestion = "Humidity is low. Consider humidifying if you feel dry.";
    }

    if (snapshot.contains("voc_trend") && snapshot.at("voc_trend").is_string() &&
        snapshot.at("voc_trend").as_string() == "rising") {
        level = "stuffy";
        suggestion = "Gas resistance is falling, so possible VOC or odor load may be rising. Ventilation may help.";
    }

    return {
        {"available", true},
        {"comfort", level},
        {"suggestion", suggestion},
        {"snapshot", snapshot},
    };
}
