/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "brookesia/system_super/macro_configs.h"
#if !BROOKESIA_SYSTEM_SUPER_ENABLE_DEBUG_LOG
#   define BROOKESIA_LOG_DISABLE_DEBUG_TRACE 1
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "boost/json.hpp"
#include "brookesia/lib_utils/describe_helpers.hpp"
#include "brookesia/service_helper/network/sntp.hpp"
#include "brookesia/service_helper/network/wifi.hpp"
#include "brookesia/service_helper/system/device.hpp"
#include "private/shell_app.hpp"
#include "private/system_constants.hpp"
#include "private/utils.hpp"

namespace esp_brookesia::system::super {
namespace {

using SNTPHelper = service::helper::SNTP;
using WifiHelper = service::helper::Wifi;
using DeviceHelper = service::helper::Device;

struct WifiStatusState {
    bool visible = false;
    bool connected = false;
};

WifiStatusState get_wifi_status_from_state(std::string_view state)
{
    if (state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Connected)) {
        return {
            .visible = true,
            .connected = true,
        };
    }
    if (state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Started) ||
            state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Connecting) ||
            state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Disconnecting)) {
        return {
            .visible = true,
            .connected = false,
        };
    }
    return {};
}

WifiStatusState get_wifi_status_from_event(std::string_view event)
{
    if (event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Connected)) {
        return {
            .visible = true,
            .connected = true,
        };
    }
    if (event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Started) ||
            event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Disconnected)) {
        return {
            .visible = true,
            .connected = false,
        };
    }
    return {};
}

std::string bool_to_binding(bool value)
{
    return value ? "true" : "false";
}

std::string make_clock_text()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    char buffer[8] = {};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M", &local_time) == 0) {
        return "--:--";
    }
    return buffer;
}

} // namespace

bool ShellApp::ensure_wifi_service_binding()
{
    if (wifi_service_binding_.is_valid()) {
        if (WifiHelper::is_running()) {
            return true;
        }
        ++wifi_status_generation_;
        set_status_wifi_state(false);
        return false;
    }
    if (!WifiHelper::is_available()) {
        ++wifi_status_generation_;
        set_status_wifi_state(false);
        return false;
    }
    wifi_service_binding_ = service::ServiceManager::get_instance().bind(WifiHelper::get_name().data());
    if (!wifi_service_binding_.is_valid()) {
        BROOKESIA_LOGW("Failed to bind Wi-Fi service for Shell status");
        ++wifi_status_generation_;
        set_status_wifi_state(false);
        return false;
    }
    if (!WifiHelper::is_running()) {
        ++wifi_status_generation_;
        wifi_service_binding_.release();
        set_status_wifi_state(false);
        return false;
    }

    if (!wifi_event_connection_.connected()) {
        const auto general_callback = [this](const std::string &, const std::string & event, bool unexpected) {
            if (unexpected) {
                BROOKESIA_LOGW("Wi-Fi service reported unexpected event for Shell status: %1%", event);
            }
            ++wifi_status_generation_;
            const auto status = get_wifi_status_from_event(event);
            set_status_wifi_state(status.connected);
        };
        wifi_event_connection_ = WifiHelper::subscribe_event(
                                     WifiHelper::EventId::GeneralEventHappened,
                                     general_callback
                                 );
        if (!wifi_event_connection_.connected()) {
            BROOKESIA_LOGW("Failed to subscribe Wi-Fi general event for Shell status");
        }
    }
    return true;
}

void ShellApp::release_wifi_service_binding()
{
    ++wifi_status_generation_;
    wifi_event_connection_.disconnect();
    wifi_service_binding_.release();
    wifi_connected_ = false;
    set_status_wifi_state(false);
}

void ShellApp::refresh_wifi_status()
{
    if (!ensure_wifi_service_binding()) {
        return;
    }

    const auto generation = ++wifi_status_generation_;
    const auto state_handler = [this, generation](service::FunctionResult && result) {
        if (context_ == nullptr || generation != wifi_status_generation_ || !wifi_service_binding_.is_valid()) {
            return;
        }
        if (!result.success || !result.has_data()) {
            set_status_wifi_state(false);
            return;
        }
        const auto &state = result.get_data<std::string>();
        const auto status = get_wifi_status_from_state(state);
        set_status_wifi_state(status.connected);
    };
    if (!WifiHelper::call_function_async(WifiHelper::FunctionId::GetGeneralState, state_handler)) {
        BROOKESIA_LOGW("Failed to submit Wi-Fi state request for Shell status");
        set_status_wifi_state(false);
    }
}

void ShellApp::set_status_wifi_state(bool connected)
{
    wifi_connected_ = connected;
    if (context_ == nullptr) {
        return;
    }
    std::vector<gui::BindingValueUpdate> updates;
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_WIFI_PATH,
        .key = "wifi_hidden",
        .value = bool_to_binding(false),
    });
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_WIFI_PATH,
        .key = "wifi_icon",
        .value = std::string(connected ? SUPER_STATUS_WIFI_ICON_CONNECTED : SUPER_STATUS_WIFI_ICON_DISCONNECTED),
    });
    auto result = context_->gui().set_binding_values(updates);
    if (!result) {
        BROOKESIA_LOGW("Failed to refresh Shell Wi-Fi status icon: %1%", result.error());
    }
}

bool ShellApp::ensure_device_service_binding()
{
    if (device_service_binding_.is_valid()) {
        return true;
    }
    if (!DeviceHelper::is_available()) {
        BROOKESIA_LOGD("Device service is not available for Shell status battery");
        return false;
    }
    device_service_binding_ = service::ServiceManager::get_instance().bind(DeviceHelper::get_name().data());
    if (!device_service_binding_.is_valid()) {
        BROOKESIA_LOGW("Failed to bind Device service for Shell status battery");
        return false;
    }
    return true;
}

void ShellApp::disconnect_device_events()
{
    device_event_connection_.disconnect();
}

void ShellApp::subscribe_device_events()
{
    disconnect_device_events();
    if (!ensure_device_service_binding()) {
        return;
    }
    const auto callback = [this](const std::string &, const service::EventItemMap &) {
        refresh_battery_status();
    };
    device_event_connection_ =
        DeviceHelper::subscribe_event(DeviceHelper::EventId::PowerBatteryStateChanged, callback);
    if (!device_event_connection_.connected()) {
        BROOKESIA_LOGW("Failed to subscribe Device battery state event for Shell status");
    }
}

void ShellApp::release_device_service_binding()
{
    ++battery_status_generation_;
    disconnect_device_events();
    device_service_binding_.release();
    set_status_battery_state(false, std::nullopt, false);
}

void ShellApp::refresh_battery_status()
{
    if (!ensure_device_service_binding()) {
        set_status_battery_state(false, std::nullopt, false);
        return;
    }
    const auto generation = ++battery_status_generation_;
    const auto state_handler = [this, generation](service::FunctionResult &&result) {
        if (context_ == nullptr || generation != battery_status_generation_) {
            return;
        }
        if (!result.success || !result.has_data()) {
            set_status_battery_state(false, std::nullopt, false);
            return;
        }

        const auto &state = result.get_data<boost::json::object>();
        bool present = false;
        std::optional<int32_t> percentage;
        bool charging = false;

        if (const auto *value = state.if_contains("is_present"); value != nullptr && value->is_bool()) {
            present = value->as_bool();
        }
        if (const auto *value = state.if_contains("percentage"); value != nullptr) {
            if (value->is_int64()) {
                percentage = static_cast<int32_t>(value->as_int64());
            } else if (value->is_uint64()) {
                percentage = static_cast<int32_t>(value->as_uint64());
            }
        }
        if (const auto *value = state.if_contains("charge_state"); value != nullptr && value->is_string()) {
            const std::string charge_state(value->as_string().data(), value->as_string().size());
            charging = (charge_state == "Charging") || (charge_state == "Trickle") ||
                       (charge_state == "PreCharge") || (charge_state == "ConstantCurrent") ||
                       (charge_state == "ConstantVoltage");
        }
        set_status_battery_state(present, percentage, charging);
    };
    if (!DeviceHelper::call_function_async(DeviceHelper::FunctionId::GetPowerBatteryState, state_handler)) {
        BROOKESIA_LOGW("Failed to submit battery state request for Shell status");
        set_status_battery_state(false, std::nullopt, false);
    }
}

void ShellApp::set_status_battery_state(bool present, std::optional<int32_t> percentage, bool charging)
{
    if (context_ == nullptr) {
        return;
    }

    const bool available = present && percentage.has_value();
    std::vector<gui::BindingValueUpdate> updates;
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_BATTERY_PATH,
        .key = "battery_hidden",
        .value = bool_to_binding(false),
    });

    std::string background = "${color.border.strong}";
    std::string text_color = "${color.text.inverse}";
    std::string text = "--%";
    if (available) {
        const auto percent = static_cast<int32_t>(std::clamp<int32_t>(*percentage, 0, 100));
        background = charging ? "${color.warning.fill}" : "${color.success.fill}";
        text_color = charging ? "${color.warning.on}" : "${color.success.on}";
        text = std::to_string(percent) + "%";
    }

    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_BATTERY_PATH,
        .key = "battery_bg",
        .value = std::move(background),
    });
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_BATTERY_LABEL_PATH,
        .key = "battery_text",
        .value = std::move(text),
    });
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_BATTERY_LABEL_PATH,
        .key = "battery_text_color",
        .value = std::move(text_color),
    });

    auto result = context_->gui().set_binding_values(updates);
    if (!result) {
        BROOKESIA_LOGW("Failed to refresh Shell battery status icon: %1%", result.error());
    }
}

bool ShellApp::ensure_sntp_service_binding()
{
    if (sntp_service_binding_.is_valid()) {
        return true;
    }
    if (!SNTPHelper::is_available()) {
        BROOKESIA_LOGD("SNTP service is not available for Shell status clock updates");
        return false;
    }

    sntp_service_binding_ = service::ServiceManager::get_instance().bind(SNTPHelper::get_name().data());
    if (!sntp_service_binding_.is_valid()) {
        BROOKESIA_LOGW("Failed to bind SNTP service for Shell status clock updates");
        return false;
    }
    return true;
}

void ShellApp::release_sntp_service_binding()
{
    disconnect_sntp_events();
    sntp_service_binding_.release();
}

void ShellApp::subscribe_sntp_events()
{
    disconnect_sntp_events();
    if (!ensure_sntp_service_binding()) {
        return;
    }

    const auto timezone_callback = [this](const std::string &, const service::EventItemMap &) {
        BROOKESIA_LOGI("SNTP timezone changed, refresh Shell status clock");
        refresh_status_clock();
        stop_status_clock_timer();
        schedule_status_clock_timer();
    };
    sntp_event_connection_ = SNTPHelper::subscribe_event(
                                 SNTPHelper::EventId::TimezoneChanged,
                                 timezone_callback
                             );
    if (!sntp_event_connection_.connected()) {
        BROOKESIA_LOGW("Failed to subscribe SNTP timezone event for Shell status clock");
    }
}

void ShellApp::disconnect_sntp_events()
{
    sntp_event_connection_.disconnect();
}

void ShellApp::refresh_status_clock()
{
    if (context_ == nullptr) {
        return;
    }
    auto result = context_->gui().set_binding_value(SUPER_STATUS_CLOCK_PATH, "clock_text", make_clock_text());
    if (!result) {
        BROOKESIA_LOGW("Failed to refresh Shell status clock: %1%", result.error());
    }
}

void ShellApp::schedule_status_clock_timer()
{
    if (context_ == nullptr || status_clock_timer_id_ != core::INVALID_TIMER_ID) {
        return;
    }
    auto timer = context_->timer().start_delayed(SUPER_STATUS_CLOCK_TIMER_NAME, 1000);
    if (!timer) {
        BROOKESIA_LOGW("Failed to start Shell status clock timer: %1%", timer.error());
        return;
    }
    status_clock_timer_id_ = *timer;
}

void ShellApp::stop_status_clock_timer()
{
    if (context_ != nullptr && status_clock_timer_id_ != core::INVALID_TIMER_ID) {
        (void)context_->timer().stop(status_clock_timer_id_);
    }
    status_clock_timer_id_ = core::INVALID_TIMER_ID;
}

} // namespace esp_brookesia::system::super
