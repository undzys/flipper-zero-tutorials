/*
Princeton Listener
- Listens on AM650 / Ook650Async
*/

#include "dolphin/dolphin.h"
#include <furi.h>
#include <gui/gui.h>
#include <string.h>

#include <furi_hal.h>
#include <lib/subghz/protocols/protocol_items.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/devices.h>

#define TAG "PrincetonListener"

typedef enum {
    EventTypeKey,
    EventTypeRxData,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

typedef struct AppState AppState;
struct AppState {
    FuriMessageQueue* queue;

    FuriMutex* mutex;
    bool has_signal;
    uint32_t serial_number;
    uint8_t button;
};

typedef void (*SubghzPacketCallback)(FuriString* buffer, void* context);

typedef enum {
    SUBGHZ_RECEIVER_INITIALIZING,
    SUBGHZ_RECEIVER_LISTENING,
    SUBGHZ_RECEIVER_NOTLISTENING,
    SUBGHZ_RECEIVER_UNINITIALING,
    SUBGHZ_RECEIVER_UNINITIALIZED,
} SubghzReceiverState;

typedef struct ListenSubGhz ListenSubGhz;
struct ListenSubGhz {
    SubGhzEnvironment* environment;
    FuriStreamBuffer* stream;
    FuriThread* thread;
    SubGhzReceiver* receiver;
    bool overrun;
    uint32_t frequency;
    FuriHalSubGhzPreset preset;
    SubghzReceiverState status;
    SubghzPacketCallback callback;
    void* callback_context;
};

// For a custom protocol, you would do something like the following...
// const SubGhzProtocol* subghz_protocol_registry_item_genie[] = {
//     &subghz_protocol_genie,
// };
// const SubGhzProtocolRegistry subghz_protocol_registry_genie = {
//     .items = subghz_protocol_registry_item_genie,
//     .size = COUNT_OF(subghz_protocol_registry_item_genie)};

static SubGhzEnvironment* load_environment() {
    SubGhzEnvironment* environment = subghz_environment_alloc();
    // subghz_environment_set_protocol_registry(environment, (void*)&subghz_protocol_registry_genie);
    subghz_environment_set_protocol_registry(environment, (void*)&subghz_protocol_registry);
    return environment;
}

ListenSubGhz* listen_subghz_alloc(
    uint32_t frequency,
    FuriHalSubGhzPreset preset,
    SubghzPacketCallback callback,
    void* callback_context) {
    ListenSubGhz* subghz = malloc(sizeof(ListenSubGhz));
    subghz->status = SUBGHZ_RECEIVER_UNINITIALIZED;
    subghz->environment = load_environment();
    subghz->stream = furi_stream_buffer_alloc(sizeof(LevelDuration) * 1024, sizeof(LevelDuration));
    furi_check(subghz->stream);
    subghz->overrun = false;
    subghz->callback = callback;
    subghz->callback_context = callback_context;
    subghz->frequency = frequency;
    subghz->preset = preset;
    return subghz;
}

void listen_subghz_free(ListenSubGhz* subghz) {
    subghz_environment_free(subghz->environment);
    furi_stream_buffer_free(subghz->stream);
    free(subghz);
}

static void
    rx_callback(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder_base, void* ctx) {
    ListenSubGhz* context = (ListenSubGhz*)ctx;

    FuriString* buffer = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder_base, buffer);
    subghz_receiver_reset(receiver);

    if(context->callback) {
        context->callback(buffer, context->callback_context);
    }

    furi_string_free(buffer);
}

static void rx_capture_callback(bool level, uint32_t duration, void* context) {
    ListenSubGhz* instance = context;

    LevelDuration level_duration = level_duration_make(level, duration);
    if(instance->overrun) {
        instance->overrun = false;
        level_duration = level_duration_reset();
    }
    size_t ret =
        furi_stream_buffer_send(instance->stream, &level_duration, sizeof(LevelDuration), 0);
    if(sizeof(LevelDuration) != ret) {
        instance->overrun = true;
    }
}

static int32_t listen_rx(void* ctx) {
    ListenSubGhz* context = (ListenSubGhz*)ctx;
    context->status = SUBGHZ_RECEIVER_LISTENING;
    LevelDuration level_duration;
    FURI_LOG_I(TAG, "listen_rx started...");
    while(context->status == SUBGHZ_RECEIVER_LISTENING) {
        int ret = furi_stream_buffer_receive(
            context->stream, &level_duration, sizeof(LevelDuration), 10);

        if(ret == sizeof(LevelDuration)) {
            if(level_duration_is_reset(level_duration)) {
                subghz_receiver_reset(context->receiver);
            } else {
                bool level = level_duration_get_level(level_duration);
                uint32_t duration = level_duration_get_duration(level_duration);
                subghz_receiver_decode(context->receiver, level, duration);
            }
        }
    }
    FURI_LOG_I(TAG, "listen_rx exiting...");
    context->status = SUBGHZ_RECEIVER_NOTLISTENING;
    return 0;
}

void start_listening(ListenSubGhz* context) {
    context->status = SUBGHZ_RECEIVER_INITIALIZING;
    subghz_devices_init();
    const SubGhzDevice* device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!subghz_devices_is_frequency_valid(device, context->frequency)) {
        FURI_LOG_E(TAG, "Frequency not in range. %lu\r\n", context->frequency);
        subghz_devices_deinit();
        return;
    }

    context->receiver = subghz_receiver_alloc_init(context->environment);
    subghz_receiver_set_filter(context->receiver, SubGhzProtocolFlag_Decodable);
    subghz_receiver_set_rx_callback(context->receiver, rx_callback, context);
    subghz_devices_begin(device);
    subghz_devices_reset(device);
    subghz_devices_load_preset(device, context->preset, NULL);
    uint32_t frequency = subghz_devices_set_frequency(device, context->frequency);
    furi_hal_power_suppress_charge_enter();
    subghz_devices_start_async_rx(device, rx_capture_callback, context);
    FURI_LOG_I(TAG, "Listening at frequency: %lu\r\n", frequency);
    context->thread = furi_thread_alloc_ex("listen-rx", 1024, listen_rx, context);
    furi_thread_start(context->thread);
}

void stop_listening(ListenSubGhz* context) {
    if(context->status == SUBGHZ_RECEIVER_UNINITIALIZED) {
        return;
    }

    context->status = SUBGHZ_RECEIVER_UNINITIALING;
    FURI_LOG_D(TAG, "Stopping listening...");
    furi_thread_join(context->thread);
    furi_thread_free(context->thread);
    furi_hal_power_suppress_charge_exit();
    const SubGhzDevice* device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    subghz_devices_stop_async_rx(device);
    subghz_receiver_free(context->receiver);
    subghz_devices_deinit();
    context->status = SUBGHZ_RECEIVER_UNINITIALIZED;
}

void subghz_packet_callback(FuriString* buffer, void* context) {
    AppState* app_state = (AppState*)context;

    FURI_LOG_I(TAG, "Decoded packet: %s\r\n", furi_string_get_cstr(buffer));
    if(furi_string_search_str(buffer, "Princeton") < furi_string_size(buffer)) {
        FURI_LOG_E(TAG, "Princeton packet detected!");

        // Look for the part that says... "Key:0x00793954"
        size_t key_index = furi_string_search_str(buffer, "Key:");
        if(key_index > 0) {
            furi_mutex_acquire(app_state->mutex, FuriWaitForever);
            FuriString* key = furi_string_alloc();
            furi_string_set_n(key, buffer, key_index + 4, 10);
            uint64_t key_value = strtoul(furi_string_get_cstr(key), NULL, 16);
            // The last 4 bits are the button, the rest is the serial number
            app_state->serial_number = (key_value >> 4) & 0x7FFFFFFF;
            app_state->button = key_value & 0xF;
            app_state->has_signal = true;
            furi_string_free(key);
            furi_mutex_release(app_state->mutex);
        } else {
            FURI_LOG_E(TAG, "Could not find key in packet!");
            // TODO: Should we still queue an RxData event?
        }

        FuriMessageQueue* queue = app_state->queue;
        AppEvent event = {.type = EventTypeRxData};
        furi_message_queue_put(queue, &event, 0);
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    AppState* data = context;

    furi_mutex_acquire(data->mutex, FuriWaitForever);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 5, 12, "433.92 MHz AM650");

    char buf[40];
    if(data->has_signal) {
        snprintf(buf, sizeof(buf), "SN:0x%07lX Btn:0x%X", data->serial_number, data->button);
        canvas_draw_str(canvas, 5, 42, buf);
    } else {
        canvas_draw_str(canvas, 5, 42, "Listening for Princeton...");
    }

    furi_mutex_release(data->mutex);
}

static void input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* queue = context;
    AppEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(queue, &event, FuriWaitForever);
}

int32_t priceton_listener_app() {
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(AppEvent));

    AppState* app_state = malloc(sizeof(AppState));
    memset(app_state, 0, sizeof(AppState));
    app_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app_state->queue = queue;

    ListenSubGhz* subghz = listen_subghz_alloc(
        433920000, FuriHalSubGhzPresetOok650Async, subghz_packet_callback, app_state);
    start_listening(subghz);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, app_state);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    dolphin_deed(DolphinDeedPluginGameStart);

    AppEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(queue, &event, FuriWaitForever) == FuriStatusOk) {
            furi_mutex_acquire(app_state->mutex, FuriWaitForever);

            if(event.type == EventTypeKey) {
                if((event.input.type == InputTypeShort || event.input.type == InputTypeRepeat) &&
                   event.input.key == InputKeyBack) {
                    running = false;
                }
            } else if(event.type == EventTypeRxData) {
                // Our data model was updated.
            }

            furi_mutex_release(app_state->mutex);
            view_port_update(view_port);
        }
    }

    FURI_LOG_D(TAG, "Exiting app, cleaning up resources...");

    // Cleanup
    stop_listening(subghz);
    listen_subghz_free(subghz);

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(queue);
    furi_mutex_free(app_state->mutex);
    free(app_state);

    return 0;
}
