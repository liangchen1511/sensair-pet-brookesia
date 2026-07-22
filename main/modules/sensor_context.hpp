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

class SensorContext {
public:
    enum class Status {
        Stopped,
        Waiting,
        Ready,
        NotDetected,
        BusUnavailable,
        InitFailed,
        ReadFailed,
    };

    struct ViewData {
        Status status = Status::Stopped;
        bool has_sample = false;
        bool sensor_online = false;
        bool gas_valid = false;
        float temperature_c = 0;
        float humidity_percent = 0;
        float pressure_hpa = 0;
        float gas_resistance_ohm = 0;
        int64_t updated_ms_ago = 0;
        size_t sample_count = 0;
        uint8_t i2c_address = 0;
        std::string voc_trend = "unknown";
        std::string reason = "not started";
    };

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
    ViewData get_view_data() const;
    bool request_sample();

    static std::vector<std::string> get_mcp_function_names();

private:
    struct Sample {
        int64_t timestamp_ms = 0;
        float temperature_c = 0;
        float humidity_percent = 0;
        float pressure_hpa = 0;
        float gas_resistance_ohm = 0;
        bool gas_valid = false;
        bool valid = false;
    };

    SensorContext() = default;
    ~SensorContext() = default;
    SensorContext(const SensorContext &) = delete;
    SensorContext &operator=(const SensorContext &) = delete;

    bool register_service_functions();
    bool init_bme690();
    bool read_bme690_once(Sample &sample);
    void sample_once();
    void push_sample(const Sample &sample);
    void set_status(Status status, std::string reason);
    std::vector<Sample> copy_samples_locked() const;

    std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler_;
    std::vector<esp_brookesia::service::ServiceBinding> service_bindings_;
    esp_brookesia::lib_utils::TaskScheduler::TaskId periodic_task_id_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> bme_available_{false};
    std::atomic<bool> sample_request_pending_{false};
    std::mutex sensor_io_mutex_;
    Status status_ = Status::Stopped;
    uint8_t detected_address_ = 0;
    std::string unavailable_reason_ = "not started";

    mutable std::mutex samples_mutex_;
    std::array<Sample, 20> samples_{};
    size_t sample_write_index_ = 0;
    size_t sample_count_ = 0;
};
