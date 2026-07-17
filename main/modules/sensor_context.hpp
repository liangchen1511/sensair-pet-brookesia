/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "boost/json.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class SensorContext {
public:
    static SensorContext &get_instance()
    {
        static SensorContext instance;
        return instance;
    }

    bool start(std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler);
    void stop();

    bool is_running() const
    {
        return running_.load();
    }

    boost::json::object get_snapshot_json() const;
    boost::json::object get_trend_json() const;
    boost::json::object get_comfort_json() const;

    static std::vector<std::string> get_mcp_function_names();

private:
    struct Sample {
        int64_t timestamp_ms = 0;
        float temperature_c = 0;
        float humidity_percent = 0;
        float pressure_hpa = 0;
        float gas_resistance_ohm = 0;
        bool valid = false;
    };

    SensorContext() = default;
    ~SensorContext() = default;
    SensorContext(const SensorContext &) = delete;
    SensorContext &operator=(const SensorContext &) = delete;

    bool register_service_functions();
    bool init_bme690();
    bool read_bme690_once(Sample &sample);
    void sample_task();
    void push_sample(const Sample &sample);
    std::vector<Sample> copy_samples_locked() const;

    static void sample_task_entry(void *arg);

    std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler_;
    std::vector<esp_brookesia::service::ServiceBinding> service_bindings_;
    TaskHandle_t sample_task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> bme_available_{false};
    std::string unavailable_reason_ = "not started";

    mutable std::mutex samples_mutex_;
    std::array<Sample, 20> samples_{};
    size_t sample_write_index_ = 0;
    size_t sample_count_ = 0;
};
