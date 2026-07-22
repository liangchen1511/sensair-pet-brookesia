/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "sensor_context.hpp"

#include <cmath>
#include "bme69x_port.hpp"
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
}

using namespace esp_brookesia;

namespace {
constexpr const char *TAG = "SensorContext";
constexpr gpio_num_t BME690_SDO_PIN = GPIO_NUM_9;
constexpr i2c_port_t I2C_MASTER_NUM = I2C_NUM_0;
constexpr uint32_t SAMPLE_INTERVAL_MS = 30000;

constexpr const char *FN_GET_ENVIRONMENT_STATUS = "GetEnvironmentStatus";
constexpr const char *SENSOR_TASK_GROUP = "SensorContext";

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

const char *status_to_string(SensorContext::Status status)
{
    switch (status) {
    case SensorContext::Status::Stopped:
        return "stopped";
    case SensorContext::Status::Waiting:
        return "waiting";
    case SensorContext::Status::Ready:
        return "ready";
    case SensorContext::Status::NotDetected:
        return "not_detected";
    case SensorContext::Status::BusUnavailable:
        return "bus_unavailable";
    case SensorContext::Status::InitFailed:
        return "init_failed";
    case SensorContext::Status::ReadFailed:
        return "read_failed";
    default:
        return "unknown";
    }
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
        set_status(Status::Stopped, "task scheduler is null");
        return false;
    }
    task_scheduler_ = std::move(task_scheduler);

    if (!register_service_functions()) {
        BROOKESIA_LOGW("Failed to register environment service functions");
    }

    set_status(Status::Waiting, "waiting for BME690 detection");
    running_.store(true);

    // Reuse the existing backend worker instead of reserving another 5 KiB
    // internal-RAM FreeRTOS task stack. Posting the first sample also lets the
    // rest of startup (especially display and XiaoZhi setup) finish first.
    if (!request_sample() ||
        !task_scheduler_->post_periodic([this]() -> bool {
            if (!running_.load()) {
                return false;
            }
            sample_once();
            return running_.load();
        }, SAMPLE_INTERVAL_MS, &periodic_task_id_, SENSOR_TASK_GROUP)) {
        running_.store(false);
        task_scheduler_->cancel_group(SENSOR_TASK_GROUP);
        sample_request_pending_.store(false);
        set_status(Status::Stopped, "failed to schedule sensor sampling");
        BROOKESIA_LOGE("Failed to schedule sensor sampling");
        return false;
    }

    BROOKESIA_LOGI("Sensor context sampling scheduled on backend worker");
    return true;
}

void SensorContext::stop()
{
    running_.store(false);
    if (task_scheduler_) {
        task_scheduler_->cancel_group(SENSOR_TASK_GROUP);
    }
    periodic_task_id_ = 0;
    std::lock_guard io_lock(sensor_io_mutex_);
    bme69x_port_deinit();
    bme_available_.store(false);
    sample_request_pending_.store(false);
    set_status(Status::Stopped, "stopped");
}

bool SensorContext::request_sample()
{
    if (!running_.load() || task_scheduler_ == nullptr) {
        return false;
    }

    bool expected = false;
    if (!sample_request_pending_.compare_exchange_strong(expected, true)) {
        return true;
    }

    const bool posted = task_scheduler_->post([this]() {
        if (running_.load()) {
            sample_once();
        }
        sample_request_pending_.store(false);
    }, nullptr, SENSOR_TASK_GROUP);
    if (!posted) {
        sample_request_pending_.store(false);
    }
    return posted;
}

std::vector<std::string> SensorContext::get_mcp_function_names()
{
    return {
        FN_GET_ENVIRONMENT_STATUS,
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

    return custom_service.register_function(
        {
            .name = FN_GET_ENVIRONMENT_STATUS,
            .description =
                "Get local temperature, humidity, pressure, gas resistance, VOC trend and comfort status from BME690.",
            .parameters = {},
            .require_scheduler = false,
        },
        [](const service::FunctionParameterMap &) -> service::FunctionResult {
            auto &context = SensorContext::get_instance();
            auto snapshot = context.get_snapshot_json();
            if (!snapshot.contains("available") || !snapshot.at("available").as_bool()) {
                return ok_json(std::move(snapshot));
            }
            boost::json::object result;
            result["available"] = true;
            result["snapshot"] = std::move(snapshot);
            result["trend"] = context.get_trend_json();
            result["comfort"] = context.get_comfort_json();
            return ok_json(std::move(result));
        }
    );
}

bool SensorContext::init_bme690()
{
    gpio_config_t sdo_conf = {};
    sdo_conf.pin_bit_mask = (1ULL << BME690_SDO_PIN);
    sdo_conf.mode = GPIO_MODE_OUTPUT;
    sdo_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    sdo_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    sdo_conf.intr_type = GPIO_INTR_DISABLE;
    auto ret = gpio_config(&sdo_conf);
    if (ret != ESP_OK) {
        set_status(Status::InitFailed, std::string("SDO GPIO init failed: ") + esp_err_to_name(ret));
        return false;
    }
    ret = gpio_set_level(BME690_SDO_PIN, 0);
    if (ret != ESP_OK) {
        set_status(Status::InitFailed, std::string("SDO GPIO level failed: ") + esp_err_to_name(ret));
        return false;
    }

    i2c_master_bus_handle_t i2c_bus = nullptr;
    ret = i2c_master_get_bus_handle(I2C_MASTER_NUM, &i2c_bus);
    if (ret != ESP_OK || i2c_bus == nullptr) {
        set_status(Status::BusUnavailable, std::string("I2C bus unavailable: ") + esp_err_to_name(ret));
        return false;
    }

    uint8_t detected_address = 0;
    ret = bme69x_port_init(&s_bme, i2c_bus, &detected_address);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            set_status(
                Status::NotDetected,
                "BME690 not detected at 0x76/0x77; power off before inserting sub-board"
            );
        } else {
            set_status(Status::BusUnavailable, std::string("BME690 I2C probe failed: ") + esp_err_to_name(ret));
        }
        return false;
    }

    int8_t rslt = bme69x_init(&s_bme);
    if (rslt != BME69X_OK) {
        set_status(Status::InitFailed, "BME690 register initialization failed");
        bme69x_port_deinit();
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
        set_status(Status::InitFailed, "BME690 measurement configuration failed");
        bme69x_port_deinit();
        return false;
    }

    bme69x_heatr_conf heatr_conf = {};
    heatr_conf.enable = BME69X_ENABLE;
    heatr_conf.heatr_temp = 300;
    heatr_conf.heatr_dur = 100;
    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &s_bme);
    if (rslt != BME69X_OK) {
        set_status(Status::InitFailed, "BME690 heater configuration failed");
        bme69x_port_deinit();
        return false;
    }

    bme_available_.store(true);
    {
        std::lock_guard lock(samples_mutex_);
        detected_address_ = detected_address;
    }
    set_status(Status::Waiting, "waiting for first BME690 sample");
    BROOKESIA_LOGI("BME690 initialized at 0x%02x, chip id: 0x%02x", detected_address, s_bme.chip_id);
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
    sample.gas_valid = (data.status & BME69X_GASM_VALID_MSK) != 0;
    sample.gas_resistance_ohm = sample.gas_valid ? data.gas_resistance : 0.0f;
    sample.valid = true;
    return true;
}

void SensorContext::sample_once()
{
    std::lock_guard io_lock(sensor_io_mutex_);
    if (!running_.load()) {
        return;
    }

    if (!bme_available_.load()) {
        if (!init_bme690()) {
            BROOKESIA_LOGW("BME690 unavailable: %s", get_view_data().reason.c_str());
            return;
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
        const auto port_error = bme69x_port_get_last_error();
        set_status(
            Status::ReadFailed,
            port_error == ESP_OK ?
                "BME690 returned no new data" :
                std::string("BME690 sample failed: ") + esp_err_to_name(port_error)
        );
        if (port_error != ESP_OK) {
            bme_available_.store(false);
            bme69x_port_deinit();
        }
        BROOKESIA_LOGW(
            "BME690 sample failed: %s",
            port_error == ESP_OK ? "no new data" : esp_err_to_name(port_error)
        );
    }
}

void SensorContext::push_sample(const Sample &sample)
{
    std::lock_guard lock(samples_mutex_);
    samples_[sample_write_index_] = sample;
    sample_write_index_ = (sample_write_index_ + 1) % samples_.size();
    if (sample_count_ < samples_.size()) {
        sample_count_++;
    }
    status_ = Status::Ready;
    unavailable_reason_.clear();
}

void SensorContext::set_status(Status status, std::string reason)
{
    std::lock_guard lock(samples_mutex_);
    status_ = status;
    unavailable_reason_ = std::move(reason);
    if (status == Status::Stopped || status == Status::NotDetected || status == Status::BusUnavailable) {
        detected_address_ = 0;
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
    const auto view = get_view_data();
    if (!view.has_sample) {
        auto result = make_error_object(view.reason.empty() ? "waiting for first sample" : view.reason);
        result["sensor_status"] = status_to_string(view.status);
        return result;
    }

    return {
        {"available", true},
        {"sensor_online", view.sensor_online},
        {"sensor_status", status_to_string(view.status)},
        {"temperature_c", view.temperature_c},
        {"humidity_percent", view.humidity_percent},
        {"pressure_hpa", view.pressure_hpa},
        {"gas_valid", view.gas_valid},
        {"gas_resistance_ohm", view.gas_resistance_ohm},
        {"voc_trend", view.voc_trend},
        {"updated_ms_ago", view.updated_ms_ago},
        {"reason", view.reason},
        {"note", "VOC trend is inferred from BME690 gas resistance, not an absolute ppm value."},
    };
}

SensorContext::ViewData SensorContext::get_view_data() const
{
    ViewData view;
    std::vector<Sample> samples;
    {
        std::lock_guard lock(samples_mutex_);
        view.status = status_;
        view.sensor_online = bme_available_.load();
        view.sample_count = sample_count_;
        view.i2c_address = detected_address_;
        view.reason = unavailable_reason_;
        samples.reserve(sample_count_);
        for (size_t i = 0; i < sample_count_; ++i) {
            const size_t idx = (sample_write_index_ + samples_.size() - sample_count_ + i) % samples_.size();
            if (samples_[idx].valid) {
                samples.push_back(samples_[idx]);
            }
        }
    }

    if (samples.empty()) {
        return view;
    }

    const auto &last = samples.back();
    view.has_sample = true;
    view.gas_valid = last.gas_valid;
    view.temperature_c = last.temperature_c;
    view.humidity_percent = last.humidity_percent;
    view.pressure_hpa = last.pressure_hpa;
    view.gas_resistance_ohm = last.gas_resistance_ohm;
    view.updated_ms_ago = std::max<int64_t>(0, now_ms() - last.timestamp_ms);

    if (samples.size() >= 2 && samples.front().gas_valid && last.gas_valid) {
        const auto &first = samples.front();
        const float gas_base = std::max(std::fabs(first.gas_resistance_ohm), 1.0f);
        const float gas_delta_percent = (last.gas_resistance_ohm - first.gas_resistance_ohm) * 100.0f / gas_base;
        if (gas_delta_percent < -12.0f) {
            view.voc_trend = "rising";
        } else if (gas_delta_percent > 12.0f) {
            view.voc_trend = "falling";
        } else {
            view.voc_trend = "stable";
        }
    }
    return view;
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
    };
}
