#include "button_service.h"

#include <cstdint>

#include "button_gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <initializer_list>
#include "iot_button.h"
#include "waveshare_board_config.h"

namespace button_service {
namespace {

constexpr const char* kTag = "ButtonService";
// Tap ceiling: a press shorter than this is a click, and two inside the window are a
// double-click. Shared by every key.
constexpr uint16_t kShortPressMs = 180;
// Per-key hold thresholds, matching the reference firmware's NavigationSwitch:
//   - ACTION: 500ms, the hold-to-record latency. Well clear of the tap ceiling so a
//     normal click can never start a recording by accident.
//   - UP/DOWN: 350ms before hold-to-repeat scrolling kicks in.
//   - FN: same as ACTION, so a deliberate hold is needed for its long-press action.
constexpr uint16_t kActionLongPressMs = 500;
constexpr uint16_t kNavigationLongPressMs = 350;
constexpr uint16_t kFunctionLongPressMs = 500;

struct ButtonContext {
    const char* label = nullptr;
    ButtonId id = ButtonId::kAction;
    gpio_num_t gpio = GPIO_NUM_NC;
    uint16_t long_press_ms = 0;
    button_handle_t handle = nullptr;
};

ButtonContext s_buttons[] = {
    // BOOT and radial SELECT use the Pocket Core direct GPIO poller below.
    // Keep iot_button only for UP/DOWN navigation, where its repeat classifier is useful.
    {"UP", ButtonId::kUp, WAVESHARE_BUTTON_UP_PIN, kNavigationLongPressMs, nullptr},
    {"DOWN", ButtonId::kDown, WAVESHARE_BUTTON_DOWN_PIN, kNavigationLongPressMs, nullptr},
};

constexpr uint32_t kDirectPollMs = 10;
TaskHandle_t s_action_button_task = nullptr;
TaskHandle_t s_select_button_task = nullptr;
std::atomic<bool> s_direct_poll_enabled{true};

struct DirectButtonState {
    const char* label;
    ButtonId id;
    gpio_num_t gpio;
    uint16_t long_press_ms;
    bool down = false;
    bool long_sent = false;
    int64_t pressed_at_us = 0;
};

DirectButtonState s_action_button = {
    "ACTION", ButtonId::kAction, WAVESHARE_BUTTON_ACTION_PIN, kActionLongPressMs
};
DirectButtonState s_select_button = {
    "SELECT", ButtonId::kFunction, WAVESHARE_BUTTON_FUNCTION_PIN, kFunctionLongPressMs
};

bool s_initialized = false;
EventHandler s_event_handler = nullptr;
void* s_event_handler_context = nullptr;

const char* EventName(button_event_t event)
{
    switch (event) {
        case BUTTON_PRESS_DOWN:
            return "PRESS_DOWN";
        case BUTTON_PRESS_UP:
            return "PRESS_UP";
        case BUTTON_PRESS_REPEAT:
            return "PRESS_REPEAT";
        case BUTTON_SINGLE_CLICK:
            return "SINGLE_CLICK";
        case BUTTON_DOUBLE_CLICK:
            return "DOUBLE_CLICK";
        case BUTTON_LONG_PRESS_START:
            return "LONG_PRESS_START";
        case BUTTON_LONG_PRESS_UP:
            return "LONG_PRESS_UP";
        default:
            return iot_button_get_event_str(event);
    }
}

bool ToButtonEvent(button_event_t event, ButtonEvent* out_event)
{
    if (out_event == nullptr) {
        return false;
    }

    switch (event) {
        case BUTTON_PRESS_DOWN:
            *out_event = ButtonEvent::kPressDown;
            return true;
        case BUTTON_PRESS_UP:
            *out_event = ButtonEvent::kPressUp;
            return true;
        case BUTTON_PRESS_REPEAT:
            *out_event = ButtonEvent::kPressRepeat;
            return true;
        case BUTTON_SINGLE_CLICK:
            *out_event = ButtonEvent::kSingleClick;
            return true;
        case BUTTON_DOUBLE_CLICK:
            *out_event = ButtonEvent::kDoubleClick;
            return true;
        case BUTTON_LONG_PRESS_START:
            *out_event = ButtonEvent::kLongPressStart;
            return true;
        case BUTTON_LONG_PRESS_UP:
            *out_event = ButtonEvent::kLongPressUp;
            return true;
        default:
            return false;
    }
}

void ButtonEventCallback(void* button_handle, void* user_data)
{
    const auto* context = static_cast<const ButtonContext*>(user_data);
    const button_event_t event =
        iot_button_get_event(static_cast<button_handle_t>(button_handle));
    const uint32_t pressed_ms =
        iot_button_get_pressed_time(static_cast<button_handle_t>(button_handle));

    ESP_LOGI(kTag, "%s event=%s gpio=%d pressed_ms=%lu",
             context != nullptr ? context->label : "UNKNOWN",
             EventName(event),
             context != nullptr ? context->gpio : GPIO_NUM_NC,
             static_cast<unsigned long>(pressed_ms));

    ButtonEvent app_event = ButtonEvent::kPressDown;
    if (context != nullptr && s_event_handler != nullptr &&
        ToButtonEvent(event, &app_event)) {
        ButtonEventInfo event_info = {};
        event_info.button = context->id;
        event_info.event = app_event;
        event_info.pressed_ms = pressed_ms;
        s_event_handler(event_info, s_event_handler_context);
    }
}

void EmitDirectEvent(const DirectButtonState& button, ButtonEvent event, uint32_t pressed_ms)
{
    if (s_event_handler == nullptr) {
        return;
    }
    ButtonEventInfo info = {};
    info.button = button.id;
    info.event = event;
    info.pressed_ms = pressed_ms;
    ESP_LOGI(kTag, "%s direct event=%d gpio=%d pressed_ms=%lu",
             button.label,
             static_cast<int>(event),
             static_cast<int>(button.gpio),
             static_cast<unsigned long>(pressed_ms));
    s_event_handler(info, s_event_handler_context);
}

void DirectButtonTask(void* arg)
{
    auto* button = static_cast<DirectButtonState*>(arg);
    if (button == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        if (!s_direct_poll_enabled.load(std::memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(kDirectPollMs));
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        const bool down_now = gpio_get_level(button->gpio) == 0;

        if (down_now && !button->down) {
            button->down = true;
            button->long_sent = false;
            button->pressed_at_us = now_us;
            EmitDirectEvent(*button, ButtonEvent::kPressDown, 0);
        } else if (down_now && button->down && !button->long_sent) {
            const uint32_t held_ms =
                static_cast<uint32_t>((now_us - button->pressed_at_us) / 1000);
            if (held_ms >= button->long_press_ms) {
                button->long_sent = true;
                EmitDirectEvent(*button, ButtonEvent::kLongPressStart, held_ms);
            }
        } else if (!down_now && button->down) {
            const uint32_t held_ms =
                static_cast<uint32_t>((now_us - button->pressed_at_us) / 1000);
            button->down = false;
            EmitDirectEvent(*button, ButtonEvent::kPressUp, held_ms);
            if (button->long_sent) {
                EmitDirectEvent(*button, ButtonEvent::kLongPressUp, held_ms);
            }
            button->long_sent = false;
            button->pressed_at_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(kDirectPollMs));
    }
}

esp_err_t InitDirectButtons()
{
    gpio_config_t cfg = {};
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.pin_bit_mask = (1ULL << WAVESHARE_BUTTON_ACTION_PIN) |
                       (1ULL << WAVESHARE_BUTTON_FUNCTION_PIN);
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    // Keep BOOT and radial SELECT on separate tasks. The BOOT callback can enter the
    // recording/transcription pipeline and block for a while; SELECT must remain responsive
    // even if that pipeline stalls or fails.
    if (s_action_button_task == nullptr) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            DirectButtonTask,
            "button_boot",
            3072,
            &s_action_button,
            8,
            &s_action_button_task,
            1);
        if (created != pdPASS) {
            s_action_button_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_select_button_task == nullptr) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            DirectButtonTask,
            "button_select",
            3072,
            &s_select_button,
            8,
            &s_select_button_task,
            1);
        if (created != pdPASS) {
            s_select_button_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(kTag, "Pocket Core direct GPIO buttons initialized independently: BOOT=%d SELECT=%d",
             WAVESHARE_BUTTON_ACTION_PIN, WAVESHARE_BUTTON_FUNCTION_PIN);
    return ESP_OK;
}

esp_err_t RegisterEvent(ButtonContext* context, button_event_t event)
{
    if (context == nullptr || context->handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = iot_button_register_cb(context->handle, event, nullptr,
                                           ButtonEventCallback, context);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Register %s callback for %s failed: %s",
                 EventName(event), context->label, esp_err_to_name(err));
    }
    return err;
}

esp_err_t CreateButton(ButtonContext* context)
{
    if (context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    button_config_t button_config = {};
    button_config.long_press_time = context->long_press_ms;
    button_config.short_press_time = kShortPressMs;

    button_gpio_config_t gpio_config = {};
    gpio_config.gpio_num = context->gpio;
    gpio_config.active_level = 0;
    gpio_config.enable_power_save = false;
    gpio_config.disable_pull = false;

    esp_err_t err = iot_button_new_gpio_device(&button_config, &gpio_config,
                                               &context->handle);
    if (err != ESP_OK || context->handle == nullptr) {
        ESP_LOGE(kTag, "Create %s button on GPIO%d failed: %s",
                 context->label, context->gpio, esp_err_to_name(err));
        context->handle = nullptr;
        return err == ESP_OK ? ESP_FAIL : err;
    }

    ESP_LOGI(kTag, "Created %s button on GPIO%d", context->label, context->gpio);

    esp_err_t first_error = ESP_OK;
    const button_event_t events[] = {
        BUTTON_PRESS_DOWN,
        BUTTON_PRESS_UP,
        BUTTON_PRESS_REPEAT,
        BUTTON_SINGLE_CLICK,
        BUTTON_DOUBLE_CLICK,
        BUTTON_LONG_PRESS_START,
        BUTTON_LONG_PRESS_UP,
    };
    for (button_event_t event : events) {
        err = RegisterEvent(context, event);
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
    }

    return first_error;
}

}  // namespace

esp_err_t Init()
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t first_error = InitDirectButtons();
    for (ButtonContext& button : s_buttons) {
        esp_err_t err = CreateButton(&button);
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
    }

    if (first_error != ESP_OK) {
        return first_error;
    }

    s_initialized = true;
    ESP_LOGI(kTag, "Button service initialized");
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    s_event_handler = handler;
    s_event_handler_context = context;
}

esp_err_t Suspend()
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_direct_poll_enabled.store(false, std::memory_order_relaxed);
    const esp_err_t err = iot_button_stop();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Button polling suspend failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "Button polling suspended");
    return ESP_OK;
}

esp_err_t Resume()
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = iot_button_resume();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Button polling resume failed; buttons are dead: %s",
                 esp_err_to_name(err));
        return err;
    }

    for (DirectButtonState* button : {&s_action_button, &s_select_button}) {
        button->down = false;
        button->long_sent = false;
        button->pressed_at_us = 0;
    }
    s_direct_poll_enabled.store(true, std::memory_order_relaxed);
    ESP_LOGI(kTag, "Button polling resumed");
    return ESP_OK;
}

}  // namespace button_service
