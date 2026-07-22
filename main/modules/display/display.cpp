/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "private/utils.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_helper.hpp"
#include "brookesia/expression_emote.hpp"
#include "brookesia/hal_interface.hpp"
#include "screens/settings.hpp"
#include "screens/emote.hpp"
#include "screens/sensors.hpp"
#include "display.hpp"

using namespace esp_brookesia;
using EmoteHelper = service::helper::ExpressionEmote;
using DeviceHelper = service::helper::Device;

namespace {

constexpr uint32_t BACKLIGHT_ON_DELAY_MS = 1000;
constexpr uint32_t LOAD_ASSETS_TIMEOUT_MS = 10000;
constexpr uint32_t EMOTE_PERF_REPORT_INTERVAL_US = 5 * 1000 * 1000;
constexpr size_t EMOTE_DMA_TIMESTAMP_QUEUE_SIZE = 4;
constexpr float PI = 3.14159265358979323846F;
constexpr size_t TOUCH_READ_MAX_POINTS = 1;

struct EmoteFlushStats {
    std::atomic<uint32_t> submitted{0};
    std::atomic<uint32_t> completed{0};
    std::atomic<uint32_t> errors{0};
    std::array<std::atomic<uint32_t>, EMOTE_DMA_TIMESTAMP_QUEUE_SIZE> submit_times_us{};
    std::atomic<uint32_t> submit_sequence{0};
    std::atomic<uint32_t> complete_sequence{0};
    std::atomic<uint32_t> total_dma_us{0};
    std::atomic<uint32_t> max_dma_us{0};
    std::atomic<uint32_t> last_submit_us{0};
    std::atomic<uint32_t> max_submit_gap_us{0};
    uint32_t report_started_us = 0;
};

EmoteFlushStats emote_flush_stats;

void update_atomic_max(std::atomic<uint32_t> &target, uint32_t value)
{
    uint32_t current = target.load(std::memory_order_relaxed);
    while ((value > current) &&
            !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void notify_emote_flush_finished()
{
    expression::Emote::get_instance().native_notify_flush_finished();
}

void on_emote_color_transfer_done(lv_display_t *display, bool in_isr, void *user_ctx)
{
    (void)display;
    (void)in_isr;
    (void)user_ctx;

    const uint32_t complete_sequence = emote_flush_stats.complete_sequence.load(std::memory_order_relaxed);
    const uint32_t submit_sequence = emote_flush_stats.submit_sequence.load(std::memory_order_acquire);
    if (complete_sequence == submit_sequence) {
        // A non-Emote dummy draw (for example a one-time background clear) completed.
        return;
    }

    const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t started_us = emote_flush_stats
                                .submit_times_us[complete_sequence % EMOTE_DMA_TIMESTAMP_QUEUE_SIZE]
                                .load(std::memory_order_relaxed);
    emote_flush_stats.complete_sequence.store(complete_sequence + 1, std::memory_order_release);
    const uint32_t dma_us = now_us - started_us;
    emote_flush_stats.total_dma_us.fetch_add(dma_us, std::memory_order_relaxed);
    update_atomic_max(emote_flush_stats.max_dma_us, dma_us);
    emote_flush_stats.completed.fetch_add(1, std::memory_order_relaxed);
    notify_emote_flush_finished();
}

void report_emote_flush_stats_if_due(uint32_t now_us)
{
    if (emote_flush_stats.report_started_us == 0) {
        emote_flush_stats.report_started_us = now_us;
        return;
    }
    if ((now_us - emote_flush_stats.report_started_us) < EMOTE_PERF_REPORT_INTERVAL_US) {
        return;
    }

    const uint32_t submitted = emote_flush_stats.submitted.exchange(0, std::memory_order_relaxed);
    const uint32_t completed = emote_flush_stats.completed.exchange(0, std::memory_order_relaxed);
    const uint32_t errors = emote_flush_stats.errors.exchange(0, std::memory_order_relaxed);
    const uint32_t total_dma_us = emote_flush_stats.total_dma_us.exchange(0, std::memory_order_relaxed);
    const uint32_t max_dma_us = emote_flush_stats.max_dma_us.exchange(0, std::memory_order_relaxed);
    const uint32_t max_submit_gap_us = emote_flush_stats.max_submit_gap_us.exchange(0, std::memory_order_relaxed);
    const uint32_t avg_dma_us = completed ? (total_dma_us / completed) : 0;
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    BROOKESIA_LOGI(
        "Emote perf/5s: submit=%1%, done=%2%, err=%3%, DMA avg/max=%4%/%5% us, max submit gap=%6% us, "
        "internal free/largest=%7%/%8%",
        submitted, completed, errors, avg_dma_us, max_dma_us, max_submit_gap_us, internal_free, internal_largest
    );
    emote_flush_stats.report_started_us = now_us;
}

constexpr uint8_t to_area_mask(Display::GestureArea area)
{
    return static_cast<uint8_t>(area);
}

uint64_t get_current_time_ms()
{
    return static_cast<uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()
               ).count()
           );
}
} // namespace

bool Display::start(const Config &config)
{
    BROOKESIA_LOG_TRACE_GUARD();

    BROOKESIA_CHECK_NULL_RETURN(config.task_scheduler, false, "Task scheduler is null");

    auto [display_name, display_iface] = hal::get_first_interface<hal::DisplayPanelIface>();
    BROOKESIA_CHECK_NULL_RETURN(display_iface, false, "Failed to get display interface");
    display_iface_ = display_iface;

    auto [touch_name, touch_iface] = hal::get_first_interface<hal::DisplayTouchIface>();
    BROOKESIA_CHECK_NULL_RETURN(touch_iface, false, "Failed to get touch interface");
    touch_iface_ = touch_iface;

    task_scheduler_ = config.task_scheduler;
    gesture_data_ = config.gesture_data;

    // Start LVGL and expression emote first
    BROOKESIA_CHECK_FALSE_RETURN(
        start_lvgl(config.lvgl_task_core), false, "Failed to start LVGL"
    );
    BROOKESIA_CHECK_FALSE_RETURN(
        start_expression_emote(config.emote_task_core), false, "Failed to start expression emote"
    );
    BROOKESIA_CHECK_FALSE_RETURN(start_ui_state_machine(), false, "Failed to start UI state machine");

    // Start gesture detection
    BROOKESIA_CHECK_FALSE_RETURN(start_gesture(config.gesture_thread_config), false, "Failed to start gesture");
    // Monitor pressing event
    auto pressing_slot = [this](const GestureInfo & info) {
        // BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

        if (is_ui_state_action_triggered_) {
            return;
        }

        // BROOKESIA_LOGI("Gesture pressing: %1%", info);

        if ((info.direction == GestureDirection::Left) || (info.direction == GestureDirection::Right)) {
            auto action = get_ui_action_from_gesture(info);
            if (action == DisplayAction::Max) {
                return;
            }

            auto action_ret = ui_state_machine_->trigger_action(BROOKESIA_DESCRIBE_TO_STR(action));
            BROOKESIA_CHECK_FALSE_EXIT(action_ret, "Failed to trigger action");

            is_ui_state_action_triggered_ = true;
        }
    };
    ui_state_gesture_pressing_connection_ = connect_gesture_pressing_signal(pressing_slot);
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_gesture_pressing_connection_.connected(), false, "Failed to connect gesture pressing signal"
    );
    // Monitor release event
    auto release_slot = [this](const GestureInfo & info) {
        BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

        // BROOKESIA_LOGI("Gesture release: %1%", info);

        is_ui_state_action_triggered_ = false;
    };
    ui_state_gesture_release_connection_ = connect_gesture_release_signal(release_slot);
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_gesture_release_connection_.connected(), false, "Failed to connect gesture release signal"
    );

    auto delayed_task = []() {
        auto result = DeviceHelper::call_function_async(
                          DeviceHelper::FunctionId::SetDisplayBacklightOnOff, true
                      );
        BROOKESIA_CHECK_FALSE_EXIT(result, "Failed to set backlight on");
    };
    auto post_delayed_ret = task_scheduler_->post_delayed(delayed_task, BACKLIGHT_ON_DELAY_MS);
    BROOKESIA_CHECK_FALSE_RETURN(post_delayed_ret, false, "Failed to post delayed task");

    return true;
}

bool Display::start_lvgl(int core_id)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    // init esp lvgl adapter
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
#pragma GCC diagnostic pop
#if CONFIG_SENSAIR_PET_AUDIO_FIRST_RENDERING
    // XiaoZhi audio runs at priority 5 and the audio mixer at priority 20.
    // Keep LVGL below both so settings/display work can never starve audio.
    adapter_config.task_priority = 4;
#else
    adapter_config.task_priority = 6;
#endif
    adapter_config.task_core_id = core_id;
    adapter_config.tick_period_ms = 5;
    adapter_config.task_min_delay_ms = 10;
    adapter_config.task_max_delay_ms = 100;
    adapter_config.stack_in_psram = true;
    BROOKESIA_CHECK_ESP_ERR_RETURN(esp_lv_adapter_init(&adapter_config), false, "Failed to initialize LVGL adapter");

    hal::DisplayPanelIface::DriverSpecific panel_driver_specific;
    BROOKESIA_CHECK_FALSE_RETURN(
        display_iface_->get_driver_specific(panel_driver_specific), false, "Failed to get driver specific"
    );
    auto &display_info = display_iface_->get_info();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lv_adapter_display_config_t display_config {0};
    switch (panel_driver_specific.bus_type) {
    case hal::DisplayPanelIface::BusType::Generic:
        display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
                             reinterpret_cast<esp_lcd_panel_handle_t>(panel_driver_specific.panel_handle),
                             reinterpret_cast<esp_lcd_panel_io_handle_t>(panel_driver_specific.io_handle),
                             static_cast<uint16_t>(display_info.h_res),
                             static_cast<uint16_t>(display_info.v_res),
                             ESP_LV_ADAPTER_ROTATE_0
                         );
        display_config.profile.require_double_buffer = false;
        display_config.profile.buffer_height = 20;
        break;
    case hal::DisplayPanelIface::BusType::MIPI:
        display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
                             reinterpret_cast<esp_lcd_panel_handle_t>(panel_driver_specific.panel_handle),
                             reinterpret_cast<esp_lcd_panel_io_handle_t>(panel_driver_specific.io_handle),
                             static_cast<uint16_t>(display_info.h_res),
                             static_cast<uint16_t>(display_info.v_res),
                             ESP_LV_ADAPTER_ROTATE_0
                         );
        break;
    case hal::DisplayPanelIface::BusType::RGB:
        display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
                             reinterpret_cast<esp_lcd_panel_handle_t>(panel_driver_specific.panel_handle),
                             reinterpret_cast<esp_lcd_panel_io_handle_t>(panel_driver_specific.io_handle),
                             static_cast<uint16_t>(display_info.h_res),
                             static_cast<uint16_t>(display_info.v_res),
                             ESP_LV_ADAPTER_ROTATE_0
                         );
        display_config.profile.buffer_height = 10;
        break;
    default:
        BROOKESIA_LOGE("Unsupported bus type: %1%", panel_driver_specific.bus_type);
        return false;
    }
#pragma GCC diagnostic pop

    lvgl_display_ = esp_lv_adapter_register_display(&display_config);
    if (!lvgl_display_) {
        BROOKESIA_LOGE("Failed to register display");
        return false;
    }

    // Add event callback to round the coordinate to the nearest 2M or 2N+1 number if necessary
    if ((panel_driver_specific.draw_x_align_bytes > 1) || (panel_driver_specific.draw_y_align_bytes > 1)) {
        uint16_t rounder_data = (panel_driver_specific.draw_x_align_bytes << 8) |
                                panel_driver_specific.draw_y_align_bytes;
        auto rounder_event_cb = [](lv_event_t *e) {
            lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
            uint16_t rounder_data = static_cast<uint16_t>(
                                        reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(e))
                                    );
            uint8_t draw_x_align_byte = static_cast<uint8_t>((rounder_data >> 8) & 0xFF) - 1;
            uint8_t draw_y_align_byte = static_cast<uint8_t>(rounder_data & 0xFF) - 1;

            uint16_t x1 = area->x1;
            uint16_t x2 = area->x2;

            uint16_t y1 = area->y1;
            uint16_t y2 = area->y2;

            // round the start of coordinate down to the nearest 2M number
            // round the end of coordinate up to the nearest 2N+1 number
            if (draw_x_align_byte > 0) {
                area->x1 = (x1 >> draw_x_align_byte) << draw_x_align_byte;
                area->x2 = ((x2 >> draw_x_align_byte) << draw_x_align_byte) + ((1 << draw_x_align_byte) - 1);
            }
            if (draw_y_align_byte > 0) {
                area->y1 = (y1 >> draw_y_align_byte) << draw_y_align_byte;
                area->y2 = ((y2 >> draw_y_align_byte) << draw_y_align_byte) + ((1 << draw_y_align_byte) - 1);
            }
        };
        lv_display_add_event_cb(
            reinterpret_cast<lv_display_t *>(lvgl_display_), rounder_event_cb, LV_EVENT_INVALIDATE_AREA,
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(rounder_data))
        );
    }

    hal::DisplayTouchIface::DriverSpecific touch_driver_specific;
    BROOKESIA_CHECK_FALSE_RETURN(
        touch_iface_->get_driver_specific(touch_driver_specific), false, "Failed to get driver specific"
    );
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(
                reinterpret_cast<lv_display_t *>(lvgl_display_),
                reinterpret_cast<esp_lcd_touch_handle_t>(touch_driver_specific.touch_handle)
            );
#pragma GCC diagnostic pop
    lvgl_indev_ = esp_lv_adapter_register_touch(&touch_config);
    if (!lvgl_indev_) {
        BROOKESIA_LOGE("Failed to register touch");
        return false;
    }

    auto start_ret = esp_lv_adapter_start();
    BROOKESIA_CHECK_ESP_ERR_RETURN(start_ret, false, "Failed to start LVGL adapter");

    return true;
}

bool Display::start_expression_emote(int core_id)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    if (!EmoteHelper::is_available()) {
        BROOKESIA_LOGW("Emote is not available, skip initialization");
        return true;
    }

    BROOKESIA_LOGI("Initializing emote...");

    hal::DisplayPanelIface::DriverSpecific panel_driver_specific;
    BROOKESIA_CHECK_FALSE_RETURN(
        display_iface_->get_driver_specific(panel_driver_specific), false, "Failed to get driver specific"
    );
    auto &display_info = display_iface_->get_info();
    // Set emote config
    EmoteHelper::Config config{
        .h_res = display_info.h_res,
        .v_res = display_info.v_res,
        .buf_pixels = static_cast<size_t>(display_info.h_res * 16),
#if CONFIG_IDF_TARGET_ESP32C5
        // The Sensair black/white EAF clips are authored at 12 FPS. A higher render
        // clock only wakes the single C5 core without producing additional frames.
        .fps = 12,
#else
        .fps = 30,
#endif
#if CONFIG_SENSAIR_PET_AUDIO_FIRST_RENDERING
        .task_priority = 3,
#else
        .task_priority = 5,
#endif
        .task_stack = 5 * 1024,
        .task_affinity = core_id,
        .flag_swap_color_bytes = (panel_driver_specific.bus_type == hal::DisplayPanelIface::BusType::Generic),
        .flag_double_buffer = true,
        .flag_buff_dma = true,
    };
    auto result = EmoteHelper::call_function_sync(
                      EmoteHelper::FunctionId::SetConfig, BROOKESIA_DESCRIBE_TO_JSON(config).as_object()
                  );
    BROOKESIA_CHECK_FALSE_RETURN(result.has_value(), false, "Failed to set emote config: %1%", result.error());

#if CONFIG_SENSAIR_PET_AUDIO_FIRST_RENDERING
    BROOKESIA_LOGI("Audio-first display enabled: emote priority=3, C5 render cap=12 FPS, overdue EAF frames are skipped");
#endif

    // Keep the LVGL adapter as the sole owner of the panel IO callback. The adapter forwards
    // the actual DMA-complete interrupt here, so Emote can release its current draw buffer
    // without synchronously blocking the single C5 core in dummy_draw_blit().
    const esp_lv_adapter_dummy_draw_callbacks_t dummy_draw_callbacks{
        .on_enable = nullptr,
        .on_disable = nullptr,
        .on_color_trans_done = on_emote_color_transfer_done,
        .on_vsync = nullptr,
    };
    auto callbacks_ret = esp_lv_adapter_set_dummy_draw_callbacks(
                             reinterpret_cast<lv_display_t *>(lvgl_display_), &dummy_draw_callbacks, nullptr
                         );
    BROOKESIA_CHECK_ESP_ERR_RETURN(callbacks_ret, false, "Failed to register asynchronous emote flush callback");

    // Subscribe to flush ready event
    auto flush_ready_event_slot = [&](const std::string & event_name, const boost::json::object & param_json) {
        if (!esp_lv_adapter_get_dummy_draw_enabled(reinterpret_cast<lv_display_t *>(lvgl_display_))) {
            notify_emote_flush_finished();
            return;
        }

        EmoteHelper::FlushReadyEventParam param;
        auto success = BROOKESIA_DESCRIBE_FROM_JSON(param_json, param);
        if (!success) {
            emote_flush_stats.errors.fetch_add(1, std::memory_order_relaxed);
            BROOKESIA_LOGE("Failed to parse flush ready event param");
            notify_emote_flush_finished();
            return;
        }

        const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
        report_emote_flush_stats_if_due(now_us);
        const uint32_t previous_submit_us = emote_flush_stats.last_submit_us.exchange(now_us, std::memory_order_relaxed);
        if (previous_submit_us != 0) {
            update_atomic_max(emote_flush_stats.max_submit_gap_us, now_us - previous_submit_us);
        }
        const uint32_t submit_sequence = emote_flush_stats.submit_sequence.load(std::memory_order_relaxed);
        const uint32_t complete_sequence = emote_flush_stats.complete_sequence.load(std::memory_order_acquire);
        if ((submit_sequence - complete_sequence) >= EMOTE_DMA_TIMESTAMP_QUEUE_SIZE) {
            emote_flush_stats.errors.fetch_add(1, std::memory_order_relaxed);
            BROOKESIA_LOGE("Emote DMA timestamp queue overflow");
            notify_emote_flush_finished();
            return;
        }
        emote_flush_stats.submit_times_us[submit_sequence % EMOTE_DMA_TIMESTAMP_QUEUE_SIZE].store(
            now_us, std::memory_order_relaxed
        );
        emote_flush_stats.submit_sequence.store(submit_sequence + 1, std::memory_order_release);
        emote_flush_stats.submitted.fetch_add(1, std::memory_order_relaxed);

        auto ret = esp_lv_adapter_dummy_draw_blit(
                       reinterpret_cast<lv_display_t *>(lvgl_display_), param.x_start, param.y_start, param.x_end,
                       param.y_end, param.data, false
                   );
        if (ret != ESP_OK) {
            emote_flush_stats.submit_sequence.store(submit_sequence, std::memory_order_release);
            emote_flush_stats.errors.fetch_add(1, std::memory_order_relaxed);
            BROOKESIA_LOGE("Failed to submit asynchronous emote bitmap: %1%", esp_err_to_name(ret));
            notify_emote_flush_finished();
        }
    };
    static auto connection = EmoteHelper::subscribe_event(EmoteHelper::EventId::FlushReady, flush_ready_event_slot);
    BROOKESIA_CHECK_FALSE_RETURN(connection.connected(), false, "Failed to subscribe to flush ready event");

    static auto binding = service::ServiceManager::get_instance().bind(EmoteHelper::get_name().data());
    BROOKESIA_CHECK_FALSE_RETURN(binding.is_valid(), false, "Failed to bind Emote service");

    // Load emote assets
    {
        EmoteHelper::AssetSource source{
            .source = ASSETS_PARTITION_NAME,
            .type = EmoteHelper::AssetSourceType::PartitionLabel,
            .flag_enable_mmap = false,
        };
        auto result = EmoteHelper::call_function_sync(
                          EmoteHelper::FunctionId::LoadAssetsSource, BROOKESIA_DESCRIBE_TO_JSON(source).as_object(),
                          service::helper::Timeout(LOAD_ASSETS_TIMEOUT_MS)
                      );
        BROOKESIA_CHECK_FALSE_RETURN(result.has_value(), false, "Failed to load emote assets: %1%", result.error());
    }

    // Set idle event message
    {
        auto result = EmoteHelper::call_function_sync(
                          EmoteHelper::FunctionId::SetEventMessage, BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Idle)
                      );
        BROOKESIA_CHECK_FALSE_RETURN(result.has_value(), false, "Failed to set emote event message: %1%", result.error());
    }

    return true;
}

bool Display::start_gesture(const esp_brookesia::lib_utils::ThreadConfig &thread_config)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    if (!check_gesture_data(gesture_data_)) {
        auto &display_info = display_iface_->get_info();
        gesture_data_ = build_default_gesture_data(display_info.h_res, display_info.v_res);
    }
    BROOKESIA_CHECK_FALSE_RETURN(check_gesture_data(gesture_data_), false, "Invalid gesture data");

    {
        boost::lock_guard<boost::mutex> lock(gesture_mutex_);
        gesture_direction_tan_threshold_ = std::tan((static_cast<float>(gesture_data_.threshold.direction_angle) * PI) / 180.0F);
        gesture_info_ = GestureInfo{};
        gesture_touch_start_time_ms_ = 0;
        gesture_detection_started_ = false;
    }

    {
        BROOKESIA_THREAD_CONFIG_GUARD(thread_config);
        gesture_thread_ = boost::thread([this]() {
            BROOKESIA_LOG_TRACE_GUARD();
            while (true) {
                process_gesture_tick();
                boost::this_thread::sleep_for(boost::chrono::milliseconds(gesture_data_.detect_period_ms));
            }
        });
    }

    BROOKESIA_LOGI("Gesture detection started(period=%1%ms)", gesture_data_.detect_period_ms);

    return true;
}

void Display::stop_gesture()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    boost::lock_guard<boost::mutex> lock(gesture_mutex_);
    gesture_info_ = GestureInfo{};
    gesture_touch_start_time_ms_ = 0;
    gesture_detection_started_ = false;
}

uint8_t Display::get_gesture_area(int x, int y) const
{
    uint8_t area = to_area_mask(GestureArea::Center);

    auto &gesture_data = gesture_data_;
    auto &display_info = display_iface_->get_info();
    area |= (y < gesture_data.threshold.vertical_edge) ? to_area_mask(GestureArea::TopEdge) : 0;
    area |= ((static_cast<int>(display_info.v_res) - y) < gesture_data.threshold.vertical_edge) ?
            to_area_mask(GestureArea::BottomEdge) : 0;
    area |= (x < gesture_data.threshold.horizontal_edge) ? to_area_mask(GestureArea::LeftEdge) : 0;
    area |= ((static_cast<int>(display_info.h_res) - x) < gesture_data.threshold.horizontal_edge) ?
            to_area_mask(GestureArea::RightEdge) : 0;

    return area;
}

bool Display::process_gesture_tick()
{
    enum class GestureEventType {
        None,
        Press,
        Pressing,
        Release,
    };

    GestureEventType event_type = GestureEventType::None;
    GestureInfo event_info;

    bool touched = false;
    lv_point_t point{0, 0};
    {
        esp_lv_adapter_lock(-1);
        lib_utils::FunctionGuard unlock_guard([this]() {
            esp_lv_adapter_unlock();
        });

        auto indev = reinterpret_cast<lv_indev_t *>(lvgl_indev_);

        lv_indev_read(indev);
        auto state = lv_indev_get_state(indev);
        touched = (state == LV_INDEV_STATE_PRESSED);
        lv_indev_get_point(indev, &point);
    }

    auto &gesture_data = gesture_data_;
    {
        boost::lock_guard<boost::mutex> lock(gesture_mutex_);

        if (touched) {
            gesture_info_.stop_x = point.x;
            gesture_info_.stop_y = point.y;
            gesture_info_.stop_area = get_gesture_area(point.x, point.y);
        }

        if (!gesture_detection_started_ && !touched) {
            return true;
        }

        if (!gesture_detection_started_ && touched) {
            gesture_detection_started_ = true;
            gesture_touch_start_time_ms_ = get_current_time_ms();
            gesture_info_ = GestureInfo{};
            gesture_info_.start_x = point.x;
            gesture_info_.start_y = point.y;
            gesture_info_.stop_x = point.x;
            gesture_info_.stop_y = point.y;
            gesture_info_.start_area = get_gesture_area(point.x, point.y);
            gesture_info_.stop_area = gesture_info_.start_area;

            event_type = GestureEventType::Press;
            event_info = gesture_info_;
        } else {
            auto current_time_ms = get_current_time_ms();
            gesture_info_.duration_ms = static_cast<uint32_t>(current_time_ms - gesture_touch_start_time_ms_);
            gesture_info_.flags_short_duration = (gesture_info_.duration_ms < static_cast<uint32_t>(gesture_data.threshold.duration_short_ms));

            int distance_x = gesture_info_.stop_x - gesture_info_.start_x;
            int distance_y = gesture_info_.stop_y - gesture_info_.start_y;
            if ((distance_x != 0) || (distance_y != 0)) {
                gesture_info_.distance_px = std::sqrt(static_cast<float>((distance_x * distance_x) + (distance_y * distance_y)));
                gesture_info_.speed_px_per_ms = (gesture_info_.duration_ms > 0) ?
                                                (gesture_info_.distance_px / static_cast<float>(gesture_info_.duration_ms)) :
                                                std::numeric_limits<float>::infinity();
                gesture_info_.flags_slow_speed = (gesture_info_.speed_px_per_ms < gesture_data.threshold.speed_slow_px_per_ms);

                float distance_tan = (distance_x == 0) ?
                                     std::numeric_limits<float>::infinity() :
                                     (static_cast<float>(distance_y) / static_cast<float>(distance_x));
                if ((distance_tan == std::numeric_limits<float>::infinity()) ||
                        (distance_tan > gesture_direction_tan_threshold_) ||
                        (distance_tan < -gesture_direction_tan_threshold_)) {
                    if (distance_y > gesture_data.threshold.direction_vertical) {
                        gesture_info_.direction = GestureDirection::Down;
                    } else if (distance_y < -gesture_data.threshold.direction_vertical) {
                        gesture_info_.direction = GestureDirection::Up;
                    }
                } else {
                    if (distance_x > gesture_data.threshold.direction_horizon) {
                        gesture_info_.direction = GestureDirection::Right;
                    } else if (distance_x < -gesture_data.threshold.direction_horizon) {
                        gesture_info_.direction = GestureDirection::Left;
                    }
                }
            }

            event_type = touched ? GestureEventType::Pressing : GestureEventType::Release;
            event_info = gesture_info_;
            if (!touched) {
                gesture_info_ = GestureInfo{};
                gesture_touch_start_time_ms_ = 0;
                gesture_detection_started_ = false;
            }
        }
    }

    switch (event_type) {
    case GestureEventType::Press:
        gesture_press_signal_(event_info);
        break;
    case GestureEventType::Pressing:
        gesture_pressing_signal_(event_info);
        break;
    case GestureEventType::Release:
        gesture_release_signal_(event_info);
        break;
    case GestureEventType::None:
    default:
        break;
    }

    return true;
}

bool Display::check_gesture_data(const GestureData &data) const
{
    if (data.detect_period_ms == 0) {
        return false;
    }
    if ((data.threshold.direction_vertical <= 0) || (data.threshold.direction_horizon <= 0)) {
        return false;
    }
    if ((data.threshold.direction_angle == 0) || (data.threshold.direction_angle >= 90)) {
        return false;
    }
    if ((data.threshold.horizontal_edge <= 0) || (data.threshold.vertical_edge <= 0)) {
        return false;
    }
    if (data.threshold.duration_short_ms <= 0) {
        return false;
    }
    if (data.threshold.speed_slow_px_per_ms <= 0) {
        return false;
    }
    auto &display_info = display_iface_->get_info();
    if ((data.threshold.direction_horizon > static_cast<int>(display_info.h_res)) ||
            (data.threshold.horizontal_edge > static_cast<int>(display_info.h_res))) {
        return false;
    }
    if ((data.threshold.direction_vertical > static_cast<int>(display_info.v_res)) ||
            (data.threshold.vertical_edge > static_cast<int>(display_info.v_res))) {
        return false;
    }

    return true;
}

Display::GestureData Display::build_default_gesture_data(uint32_t h_res, uint32_t v_res)
{
    GestureData data;
    data.detect_period_ms = 20;
    data.threshold.direction_horizon = std::max(24, static_cast<int>(h_res / 6));
    data.threshold.direction_vertical = std::max(24, static_cast<int>(v_res / 6));
    data.threshold.direction_angle = 45;
    data.threshold.horizontal_edge = std::max(12, static_cast<int>(h_res / 10));
    data.threshold.vertical_edge = std::max(12, static_cast<int>(v_res / 10));
    data.threshold.duration_short_ms = 220;
    data.threshold.speed_slow_px_per_ms = 0.6F;
    return data;
}

bool Display::start_ui_state_machine()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_CHECK_EXCEPTION_RETURN(
        ui_state_machine_ = std::make_unique<lib_utils::StateMachine>(), false, "Failed to create state machine"
    );

    // Create states
    std::shared_ptr<ScreenSettings> screen_settings;
    std::shared_ptr<ScreenEmote> screen_emote;
    std::shared_ptr<ScreenSensors> screen_sensors;
    {
        // Lock LVGL adapter to ensure thread-safe operations
        esp_lv_adapter_lock(-1);
        lib_utils::FunctionGuard unlock_guard([this]() {
            esp_lv_adapter_unlock();
        });
        BROOKESIA_CHECK_EXCEPTION_RETURN(
            screen_settings = std::make_shared<ScreenSettings>(), false, "Failed to create state"
        );
        BROOKESIA_CHECK_EXCEPTION_RETURN(
            screen_emote = std::make_shared<ScreenEmote>(), false, "Failed to create state"
        );
        BROOKESIA_CHECK_EXCEPTION_RETURN(
            screen_sensors = std::make_shared<ScreenSensors>(), false, "Failed to create state"
        );
    }

    // Add states to state machine
    auto add_settings_ret = ui_state_machine_->add_state(screen_settings);
    BROOKESIA_CHECK_FALSE_RETURN(add_settings_ret, false, "Failed to add state");
    auto add_emote_ret = ui_state_machine_->add_state(screen_emote);
    BROOKESIA_CHECK_FALSE_RETURN(add_emote_ret, false, "Failed to add state");
    auto add_sensors_ret = ui_state_machine_->add_state(screen_sensors);
    BROOKESIA_CHECK_FALSE_RETURN(add_sensors_ret, false, "Failed to add state");

    // Add transitions between states
    auto action_scroll_left = BROOKESIA_DESCRIBE_TO_STR(DisplayAction::ScrollLeft);
    auto action_scroll_right = BROOKESIA_DESCRIBE_TO_STR(DisplayAction::ScrollRight);
    auto action_edge_scroll_left = BROOKESIA_DESCRIBE_TO_STR(DisplayAction::EdgeScrollLeft);
    auto action_edge_scroll_right = BROOKESIA_DESCRIBE_TO_STR(DisplayAction::EdgeScrollRight);
    // Finger swipe left returns from Settings to the Emote home screen.
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_settings->get_name(), action_scroll_left, screen_emote->get_name()
        ), false, "Failed to add transition"
    );
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_settings->get_name(), action_edge_scroll_left, screen_emote->get_name()
        ), false, "Failed to add transition"
    );
    // Finger swipe right returns from Sensors to the Emote home screen.
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_sensors->get_name(), action_scroll_right, screen_emote->get_name()
        ), false, "Failed to add transition"
    );
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_sensors->get_name(), action_edge_scroll_right, screen_emote->get_name()
        ), false, "Failed to add transition"
    );
    // The gestures are defined by finger movement, as agreed with the product interaction:
    // swipe right -> Settings, swipe left -> Sensors.
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_emote->get_name(), action_scroll_right, screen_settings->get_name()
        ), false, "Failed to add transition"
    );
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_emote->get_name(), action_edge_scroll_right, screen_settings->get_name()
        ), false, "Failed to add transition"
    );
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_emote->get_name(), action_scroll_left, screen_sensors->get_name()
        ), false, "Failed to add transition"
    );
    BROOKESIA_CHECK_FALSE_RETURN(
        ui_state_machine_->add_transition(
            screen_emote->get_name(), action_edge_scroll_left, screen_sensors->get_name()
        ), false, "Failed to add transition"
    );

    // Start state machine
    auto pre_execute_callback =
        [this](const lib_utils::TaskScheduler::Group & group, lib_utils::TaskScheduler::TaskId id,
    lib_utils::TaskScheduler::TaskType type) {
        BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
        esp_lv_adapter_lock(-1);
    };
    auto post_execute_callback =
        [this](const lib_utils::TaskScheduler::Group & group, lib_utils::TaskScheduler::TaskId id,
    lib_utils::TaskScheduler::TaskType type, bool success) {
        BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
        esp_lv_adapter_unlock();
    };
    auto start_ret = ui_state_machine_->start({
        .task_scheduler = task_scheduler_,
        .task_group_name = TASK_GROUP_NAME,
        .task_group_config = {
            .pre_execute_callback = pre_execute_callback,
            .post_execute_callback = post_execute_callback,
        },
        .initial_state = screen_emote->get_name(),
    });
    BROOKESIA_CHECK_FALSE_RETURN(start_ret, false, "Failed to start state machine");

    return true;
}

DisplayAction Display::get_ui_action_from_gesture(const GestureInfo &info) const
{
    if ((info.start_area & static_cast<uint8_t>(GestureArea::TopEdge)) && (info.direction == GestureDirection::Down)) {
        return DisplayAction::EdgeScrollDown;
    } else if ((info.start_area & static_cast<uint8_t>(GestureArea::BottomEdge)) &&
               (info.direction == GestureDirection::Up)) {
        return DisplayAction::EdgeScrollUp;
    } else if ((info.start_area & static_cast<uint8_t>(GestureArea::LeftEdge)) &&
               (info.direction == GestureDirection::Right)) {
        return DisplayAction::EdgeScrollRight;
    } else if ((info.start_area & static_cast<uint8_t>(GestureArea::RightEdge)) &&
               (info.direction == GestureDirection::Left)) {
        return DisplayAction::EdgeScrollLeft;
    } else {
        if (info.direction == GestureDirection::Left) {
            return DisplayAction::ScrollLeft;
        } else if (info.direction == GestureDirection::Right) {
            return DisplayAction::ScrollRight;
        } else if (info.direction == GestureDirection::Up) {
            return DisplayAction::ScrollUp;
        } else if (info.direction == GestureDirection::Down) {
            return DisplayAction::ScrollDown;
        }
    }

    return DisplayAction::Max;
}

bool Display::send_display_task(esp_brookesia::lib_utils::TaskScheduler::OnceTask &&task)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_CHECK_NULL_RETURN(task_scheduler_, false, "Task scheduler is null");

    auto result = task_scheduler_->post(std::move(task), nullptr, Display::TASK_GROUP_NAME);
    BROOKESIA_CHECK_FALSE_RETURN(result, false, "Failed to post task function");

    return true;
}
