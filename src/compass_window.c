#include "compass_window.h"
#include "ticks_layer.h"
#include "data_provider.h"
#include "inverted_cross_hair_layer.h"
#include "compass_calibration_window.h"

typedef struct {
    bool window_appeared;
    GRect direction_layer_rect_rose;
    GRect direction_layer_rect_band;
    TextLayer *direction_layer;

    GRect angle_layer_rect_rose;
    GRect angle_layer_rect_band;
    TextLayer *angle_layer;

    TicksLayer *ticks_layer;

    GRect pointer_layer_rect_rose;
    GRect pointer_layer_rect_band;
    InverterLayer *pointer_layer;

    TextLayer *accel_layer;

    DataProvider* data_provider;

    bool shows_band;
    Animation *transition_animation;
    float transition_animation_start_value;

    int64_t last_update_layout;

    BitmapLayer *large_cross_hair_layer;
    InvertedCrossHairLayer *small_cross_hair_layer;
    MagData data;

    CompassCalibrationWindow *calibration_window;
} CompassWindowData;

int64_t time_64() {
    time_t s;
    uint16_t ms;
    time_ms(&s, &ms);
    return (int64_t)s * 1000 + ms;
}

Window *compass_window_get_window(CompassWindow *window) {
    return (Window *)window;
}

static void compass_layer_update_layout(CompassWindowData *data);

static GSize size_blend(GSize s1, GSize s2, float f) {
    return (GSize){
        (int16_t) (s1.w * (1-f) + f * s2.w),
        (int16_t) (s1.h * (1-f) + f * s2.h),
    };
}

static GRect rect_blend(GRect *r1, GRect *r2, float f) {
    return (GRect){
        .origin = {
            (int16_t) (r1->origin.x * (1-f) + f * r2->origin.x),
            (int16_t) (r1->origin.y * (1-f) + f * r2->origin.y),
        },
       .size = size_blend(r1->size, r2->size, f),
    };
}

static GRect rect_centered_with_size(GRect *r1, GSize size, GPoint offset) {
    return (GRect){
            .origin = {
                    (int16_t) ((r1->size.w - size.w) / 2 + r1->origin.x + offset.x),
                    (int16_t) ((r1->size.h - size.h) / 2 + r1->origin.y + offset.y),
            },
            .size = size,
    };
}

static void compass_layer_update_layout(CompassWindowData *data) {
    data->last_update_layout = time_64();
    int32_t angle = data_provider_get_presentation_angle(data->data_provider);
    ticks_layer_set_angle(data->ticks_layer, angle);

    const float transition_factor = data_provider_get_orientation_transition_factor(data->data_provider);
    ticks_layer_set_transition_factor(data->ticks_layer, transition_factor);

    static char angle_text[] = "123°";
    int32_t normalized_angle = ((int)(angle * 360 / TRIG_MAX_ANGLE) % 360 + 360) % 360;
    snprintf(angle_text, sizeof(angle_text), "%d°", (int)normalized_angle);
    text_layer_set_text(data->angle_layer, angle_text);
    GRect r = rect_blend(&data->angle_layer_rect_rose, &data->angle_layer_rect_band, transition_factor);
    layer_set_frame(text_layer_get_layer(data->angle_layer), r);
    // workound!
    // TODO: file a bug for tintin applib
    layer_set_bounds(text_layer_get_layer(data->angle_layer), (GRect){.size=r.size});
    text_layer_set_text_alignment(data->angle_layer, GTextAlignmentRight);

    static char *direction_texts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int32_t direction_index = ((normalized_angle + 23) / (360 / ARRAY_LENGTH(direction_texts))) % ARRAY_LENGTH(direction_texts);
//    int32_t direction_index = 1;
    text_layer_set_text(data->direction_layer, direction_texts[direction_index]);
    layer_set_frame(text_layer_get_layer(data->direction_layer), rect_blend(&data->direction_layer_rect_rose, &data->direction_layer_rect_band, transition_factor));

    layer_set_frame(inverter_layer_get_layer(data->pointer_layer), rect_blend(&data->pointer_layer_rect_rose, &data->pointer_layer_rect_band, transition_factor));

    static char accel_text[40];
    AccelData ad = data_provider_last_accel_data(data->data_provider);
    snprintf(accel_text, sizeof(accel_text), "x:%d y:%d z:%d", (int)ad.x, (int)ad.y, (int)ad.z);
//    APP_LOG(APP_LOG_LEVEL_DEBUG, "%s", accel_text);
    text_layer_set_text(data->accel_layer, accel_text);

    GRect frame = layer_get_frame(ticks_layer_get_layer(data->ticks_layer));
    const GBitmap *bmp = bitmap_layer_get_bitmap(data->large_cross_hair_layer);

    // make crosses move down outside the screen when transition to cartesian representation
    int16_t transition_dy = (int16_t) (transition_factor * frame.size.h);

    layer_set_frame(bitmap_layer_get_layer(data->large_cross_hair_layer),
            rect_centered_with_size(&frame, bmp->bounds.size, GPoint(1,transition_dy)));

    int16_t d = 40;   // TODO: get rid of magic number
    GPoint small_cross_hair_offset = GPoint((int16_t)(1 - ad.x / d), ad.y / d);
    // make small cross hair stick to center during transition until almost back to polar representation
    float tf2 = (1-transition_factor) * (1-transition_factor);
    tf2 *= tf2;
    small_cross_hair_offset.x *= tf2;
    small_cross_hair_offset.y *= tf2;
    small_cross_hair_offset.y += transition_dy;

    layer_set_frame(inverted_cross_hair_layer_get_layer(data->small_cross_hair_layer),
            rect_centered_with_size(&frame, GSize(17, 17), small_cross_hair_offset));
}

static void compass_window_load(Window *window) {
    // TODO: get rid of absolute coordinates
    CompassWindowData *data = window_get_user_data(window);
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    const int16_t text_height_rose = 24;
    const int16_t text_height_band = 55;
    const GFont text_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);

    const int16_t direction_layer_width = 40;
    const int16_t direction_layer_margin_band = 40;
    data->direction_layer_rect_rose = (GRect) { .origin = { bounds.size.w - direction_layer_width, (int16_t)(bounds.size.h- text_height_rose)}, .size = { direction_layer_width, text_height_rose} };
    data->direction_layer_rect_band = (GRect) { .origin = { bounds.size.w - direction_layer_width-direction_layer_margin_band, (int16_t)(bounds.size.h- text_height_band)}, .size = { direction_layer_width, text_height_rose} };
    data->direction_layer = text_layer_create(data->direction_layer_rect_rose);
    text_layer_set_text_alignment(data->direction_layer, GTextAlignmentCenter);
    text_layer_set_font(data->direction_layer, text_font);
    text_layer_set_text_color(data->direction_layer, GColorWhite);
    text_layer_set_background_color(data->direction_layer, GColorClear);
    layer_add_child(window_layer, text_layer_get_layer(data->direction_layer));

    const int16_t angle_layer_width_rose = 40;
    const int16_t angle_layer_width_band = 65;

    data->angle_layer_rect_rose = (GRect) { .origin = { 0, (int16_t)(bounds.size.h- text_height_rose)}, .size = { angle_layer_width_rose, text_height_rose} };
    data->angle_layer_rect_band = (GRect) { .origin = { 0, (int16_t)(bounds.size.h- text_height_band)}, .size = { angle_layer_width_band, text_height_band} };
    data->angle_layer = text_layer_create(data->angle_layer_rect_rose);
    text_layer_set_text_alignment(data->angle_layer, GTextAlignmentRight);
    text_layer_set_font(data->angle_layer, text_font);
    text_layer_set_text_color(data->angle_layer, GColorWhite);
    text_layer_set_background_color(data->angle_layer, GColorClear);
    layer_add_child(window_layer, text_layer_get_layer(data->angle_layer));

    GRect roseRect = ((GRect){.origin={0, 8}, .size={bounds.size.w, (int16_t)(bounds.size.h-15)}});

    data->ticks_layer = ticks_layer_create(roseRect);
    layer_add_child(window_layer, ticks_layer_get_layer(data->ticks_layer));

    data->pointer_layer_rect_rose = (GRect){{71,0}, {3,20}};
    data->pointer_layer_rect_band = (GRect){{71,18}, {3,40}};
    data->pointer_layer = inverter_layer_create(data->pointer_layer_rect_rose);
    layer_add_child(window_layer, inverter_layer_get_layer(data->pointer_layer));


    data->accel_layer = text_layer_create((GRect){{0,40},{144, 20}});
    text_layer_set_text_color(data->angle_layer, GColorWhite);
    text_layer_set_background_color(data->angle_layer, GColorClear);
//    layer_add_child(window_layer, text_layer_get_layer(data->accel_layer));

    GBitmap *bmp = gbitmap_create_with_resource(RESOURCE_ID_CROSS_HAIR_LARGE);
    data->large_cross_hair_layer = bitmap_layer_create(GRectZero);
    bitmap_layer_set_bitmap(data->large_cross_hair_layer, bmp);
    layer_add_child(window_layer, bitmap_layer_get_layer(data->large_cross_hair_layer));

    data->small_cross_hair_layer = inverted_cross_hair_layer_create(GRectZero);
    layer_add_child(window_layer, inverted_cross_hair_layer_get_layer(data->small_cross_hair_layer));

    data_provider_set_target_angle(data->data_provider, (360-45) * TRIG_MAX_ANGLE / 360);
}

static void compass_window_appear(Window *window) {
    CompassWindowData *data = window_get_user_data(window);
    data->window_appeared = true;
}

static void compass_window_unload(Window *window) {
    CompassWindowData *data = window_get_user_data(window);
    ticks_layer_destroy(data->ticks_layer);
    text_layer_destroy(data->angle_layer);
    text_layer_destroy(data->direction_layer);
    inverter_layer_destroy(data->pointer_layer);
    text_layer_destroy(data->accel_layer);
    inverted_cross_hair_layer_destroy(data->small_cross_hair_layer);

    gbitmap_destroy((GBitmap *)bitmap_layer_get_bitmap(data->large_cross_hair_layer));
    bitmap_layer_destroy(data->large_cross_hair_layer);
}

static void handle_data_provider_update(DataProvider *provider, void* user_data) {
    CompassWindowData *data = user_data;
    if(data->calibration_window && window_stack_get_top_window() == compass_calibration_window_get_window(data->calibration_window)){
        AccelData accel_data = data_provider_get_damped_accel_data(provider);
        compass_calibration_window_apply_accel_data(data->calibration_window, accel_data);

        if(!data_provider_compass_needs_calibration(provider)) {
            window_stack_pop(true);
            vibes_long_pulse();
        }
    } else {
        compass_layer_update_layout(data);

        if(data->window_appeared && data_provider_compass_needs_calibration(provider)) {
            if(!data->calibration_window) {
                data->calibration_window = compass_calibration_window_create();
            }
            window_stack_push(compass_calibration_window_get_window(data->calibration_window), true);
        }
    }
}

CompassWindow *compass_window_create() {
    Window *window = window_create();
    CompassWindowData *data = malloc(sizeof(CompassWindowData));

    data->data_provider = data_provider_create(data, (DataProviderHandlers) {
            .presented_angle_or_accel_data_changed = handle_data_provider_update,
            .orientation_transition_factor_changed = handle_data_provider_update,
    });

    window_set_user_data(window, data);

    window_set_window_handlers(window, (WindowHandlers) {
            .load = compass_window_load,
            .unload = compass_window_unload,
            .appear = compass_window_appear,
    });
    window_set_background_color(window, GColorBlack);

    return (CompassWindow *) window;
}

void compass_window_destroy(CompassWindow *window) {
    if(!window)return;

    CompassWindowData *data = window_get_user_data((Window *)window);
    data_provider_destroy(data->data_provider);
    compass_calibration_window_destroy(data->calibration_window);
    free(data);

    window_destroy((Window *)window);
}

