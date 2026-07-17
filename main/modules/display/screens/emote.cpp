/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "esp_lv_adapter.h"
#include "private/utils.hpp"
#include "brookesia/service_helper.hpp"
#include "brookesia/agent_helper.hpp"
#include "modules/display/display.hpp"
#include "emote.hpp"

using namespace esp_brookesia;
using EmoteHelper = service::helper::ExpressionEmote;
using AgentHelper = agent::helper::Manager;
using WifiHelper = service::helper::Wifi;

namespace {

lv_obj_t *black_screen = nullptr;

bool prepare_black_screen(lv_display_t *display)
{
    auto lock_ret = esp_lv_adapter_lock(-1);
    BROOKESIA_CHECK_ESP_ERR_RETURN(lock_ret, false, "Failed to lock LVGL for black screen refresh");
    lib_utils::FunctionGuard unlock_guard([]() {
        esp_lv_adapter_unlock();
    });

    if (black_screen == nullptr) {
        black_screen = lv_obj_create(nullptr);
        BROOKESIA_CHECK_NULL_RETURN(black_screen, false, "Failed to create black transition screen");
        lv_obj_remove_flag(black_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(black_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(black_screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(black_screen, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(black_screen, 0, LV_PART_MAIN);
    }

    // EAF objects only redraw their own dirty regions. Clear the complete 240x284 GRAM first
    // so the power-on/LVGL white background cannot survive around the 240x240 animation.
    lv_screen_load(black_screen);
    lv_obj_invalidate(black_screen);
    lv_refr_now(display);
    return true;
}

} // namespace

ScreenEmote::ScreenEmote():
    StateBase(BROOKESIA_DESCRIBE_TO_STR(DisplayScreen::Emote))
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
}

ScreenEmote::~ScreenEmote()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
}

bool ScreenEmote::on_enter(const std::string &from_state, const std::string &action)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_LOGI("Entering '%1%' from '%2%' with action '%3%'", get_name(), from_state, action);

    if (AgentHelper::is_running() && WifiHelper::is_running()) {
        auto wifi_state_handler = [this](service::FunctionResult && result) {
            BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

            BROOKESIA_CHECK_FALSE_EXIT(result.success, "Failed to get WiFi state: %1%", result.error_message);

            auto &data = result.get_data<std::string>();
            WifiHelper::GeneralState wifi_state = WifiHelper::GeneralState::Max;
            auto parse_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(data, wifi_state);
            BROOKESIA_CHECK_FALSE_EXIT(parse_result, "Failed to parse WiFi state");

            auto start_agent_result = AgentHelper::call_function_async(
                                          AgentHelper::FunctionId::TriggerGeneralAction,
                                          BROOKESIA_DESCRIBE_TO_STR(AgentHelper::GeneralAction::Start)
                                      );
            BROOKESIA_CHECK_FALSE_EXIT(start_agent_result, "Failed to start agent");

            auto resume_result = AgentHelper::call_function_async(AgentHelper::FunctionId::Resume);
            BROOKESIA_CHECK_FALSE_EXIT(resume_result, "Failed to resume agent");
        };
        auto wifi_state_result = WifiHelper::call_function_async(WifiHelper::FunctionId::GetGeneralState, wifi_state_handler);
        BROOKESIA_CHECK_FALSE_RETURN(wifi_state_result, true, "Failed to get WiFi state");
    }

    lv_display_t *lv_disp = lv_display_get_default();
    BROOKESIA_CHECK_NULL_RETURN(lv_disp, false, "Failed to get default display");

    BROOKESIA_CHECK_FALSE_RETURN(prepare_black_screen(lv_disp), false, "Failed to prepare black emote background");

    auto ret = esp_lv_adapter_set_dummy_draw(lv_disp, true);
    BROOKESIA_CHECK_ESP_ERR_RETURN(ret, false, "Failed to set dummy draw");

    auto result = EmoteHelper::call_function_sync(EmoteHelper::FunctionId::RefreshAll);
    BROOKESIA_CHECK_FALSE_RETURN(result, false, "Failed to refresh emotes");

    return true;
}

bool ScreenEmote::on_exit(const std::string &to_state, const std::string &action)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_LOGI("Exiting '%1%' to '%2%' with action '%3%'", get_name(), to_state, action);

    if (AgentHelper::is_running()) {
        auto suspend_result = AgentHelper::call_function_async(AgentHelper::FunctionId::Suspend);
        BROOKESIA_CHECK_FALSE_RETURN(suspend_result, false, "Failed to suspend agent");
    }

    return true;
}
