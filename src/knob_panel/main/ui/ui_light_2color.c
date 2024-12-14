/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include <stdio.h>
#include "lv_example_pub.h"
#include "lv_example_image.h"
#include "bsp/esp-bsp.h"
#include "app_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "unity.h"
#include "iot_button.h"
#include "sdkconfig.h"
#include "freertos/semphr.h"
#include "audio_player.h"

static const char *TAG = "ui_light_2color_audio";

//Made semaphores to handle the tasks and a queue for the audio playing
SemaphoreHandle_t playback_semaphore;
QueueHandle_t playback_queue;
SemaphoreHandle_t light_update_semaphore;

// Task for the thread 
TaskHandle_t task1_handle = NULL;

static bool light_2color_layer_enter_cb(void *layer);
static bool light_2color_layer_exit_cb(void *layer);
static void light_2color_layer_timer_cb(lv_timer_t *tmr);

typedef enum
{
    LIGHT_CCK_WARM,
    LIGHT_CCK_COOL,
    LIGHT_CCK_MAX,
} LIGHT_CCK_TYPE;

typedef struct
{
    uint8_t light_pwm;
    LIGHT_CCK_TYPE light_cck;
} light_set_attribute_t;

typedef struct
{
    const lv_img_dsc_t *img_bg[2];

    const lv_img_dsc_t *img_pwm_25[2];
    const lv_img_dsc_t *img_pwm_50[2];
    const lv_img_dsc_t *img_pwm_75[2];
    const lv_img_dsc_t *img_pwm_100[2];
} ui_light_img_t;

static lv_obj_t *page;
static time_out_count time_20ms, time_500ms;

static lv_obj_t *img_light_bg, *label_pwm_set;
static lv_obj_t *img_light_pwm_25, *img_light_pwm_50, *img_light_pwm_75, *img_light_pwm_100, *img_light_pwm_0;

static light_set_attribute_t light_set_conf, light_xor;

static const ui_light_img_t light_image = {
    {&light_warm_bg, &light_cool_bg},
    {&light_warm_25, &light_cool_25},
    {&light_warm_50, &light_cool_50},
    {&light_warm_75, &light_cool_75},
    {&light_warm_100, &light_cool_100},
};

lv_layer_t light_2color_Layer = {
    .lv_obj_name = "light_2color_Layer",
    .lv_obj_parent = NULL,
    .lv_obj_layer = NULL,
    .lv_show_layer = NULL,
    .enter_cb = light_2color_layer_enter_cb,
    .exit_cb = light_2color_layer_exit_cb,
    .timer_cb = light_2color_layer_timer_cb,
};

static void light_2color_event_cb(lv_event_t *e)
{
    // Initail values for managing duplicates
    lv_event_code_t code = lv_event_get_code(e);
    PDM_SOUND_TYPE audio_level = ZERO_PERCENT;
    static PDM_SOUND_TYPE last_enqueued_audio = -1;

    if (LV_EVENT_FOCUSED == code)
    {
        lv_group_set_editing(lv_group_get_default(), true);
    }
    else if (LV_EVENT_KEY == code)
    {
        uint32_t key = lv_event_get_key(e);

        if (is_time_out(&time_500ms))
        {
            if (LV_KEY_RIGHT == key)
            {
                if (light_set_conf.light_pwm < 100)
                {
                    light_set_conf.light_pwm += 25;
                }
            }
            else if (LV_KEY_LEFT == key)
            {
                if (light_set_conf.light_pwm > 0)
                {
                    light_set_conf.light_pwm -= 25;
                }
            }
        }
    }
    else if (LV_EVENT_CLICKED == code)
    {
        light_set_conf.light_cck =
            (LIGHT_CCK_WARM == light_set_conf.light_cck) ? (LIGHT_CCK_COOL) : (LIGHT_CCK_WARM);
    }
    else if (LV_EVENT_LONG_PRESSED == code)
    {
        lv_indev_wait_release(lv_indev_get_next(NULL));
        ui_remove_all_objs_from_encoder_group();
        lv_func_goto_layer(&menu_layer);
    }

    // Set the audio levels based on the pwm
    switch (light_set_conf.light_pwm)
    {
    case 0:
        audio_level = ZERO_PERCENT;
        break;
    case 25:
        audio_level = TWENTY_FIVE_PERCENT;
        break;
    case 50:
        audio_level = FIFTY_PERCENT;
        break;
    case 75:
        audio_level = SEVENTY_FIVE_PERCENT;
        break;
    case 100:
        audio_level = ONE_HUNDRED_PERCENT;
        break;
    }

    // Enqueue the sound levels and remove a duplicate
    if (audio_level != last_enqueued_audio)
    {
        if (xQueueSend(playback_queue, &audio_level, portMAX_DELAY) == pdPASS)
        {
            ESP_LOGI(TAG, "Enqueued audio level: %d", audio_level);
            last_enqueued_audio = audio_level;
        }
        else
        {
            ESP_LOGW(TAG, "Playback queue is full. Dropping request.");
        }
    }
}

// This recieves the playback queue and then plays, when it is done unlocks the lighting which is on a seperate thread
static void play_audio_task(void *param)
{
    PDM_SOUND_TYPE current_audio;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (xQueueReceive(playback_queue, &current_audio, portMAX_DELAY))
        {
            if (xSemaphoreTake(playback_semaphore, portMAX_DELAY))
            {
                ESP_LOGI(TAG, "Playing audio: %d", current_audio);

                audio_handle_info(current_audio);
                vTaskDelay(pdMS_TO_TICKS(2000));
                xSemaphoreGive(playback_semaphore);
                xSemaphoreGive(light_update_semaphore);
            }
        }
    }
}

void ui_light_2color_init(lv_obj_t *parent)
{
    // Init all the semaphores and queue
    playback_semaphore = xSemaphoreCreateBinary();
    if (playback_semaphore == NULL)
    {
        ESP_LOGE(TAG, "Failed to create playback semaphore");
    }
    else
    {
        xSemaphoreGive(playback_semaphore); 
    }

    light_update_semaphore = xSemaphoreCreateBinary();
    if (light_update_semaphore == NULL)
    {
        ESP_LOGE(TAG, "Failed to create light update semaphore");
    }
        else
    {
        xSemaphoreGive(light_update_semaphore); 
    }

    playback_queue = xQueueCreate(10, sizeof(PDM_SOUND_TYPE));
    if (playback_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create playback queue");
    }

    light_xor.light_pwm = 0xFF;
    light_xor.light_cck = LIGHT_CCK_MAX;

    light_set_conf.light_pwm = 50;
    light_set_conf.light_cck = LIGHT_CCK_WARM;

    page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    // lv_obj_set_size(page, lv_obj_get_width(lv_obj_get_parent(page)), lv_obj_get_height(lv_obj_get_parent(page)));

    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(page);

    img_light_bg = lv_img_create(page);
    lv_img_set_src(img_light_bg, &light_warm_bg);
    lv_obj_align(img_light_bg, LV_ALIGN_CENTER, 0, 0);

    label_pwm_set = lv_label_create(page);
    lv_obj_set_style_text_font(label_pwm_set, &HelveticaNeue_Regular_24, 0);
    if (light_set_conf.light_pwm)
    {
        lv_label_set_text_fmt(label_pwm_set, "%d%%", light_set_conf.light_pwm);
    }
    else
    {
        lv_label_set_text(label_pwm_set, "--");
    }
    lv_obj_align(label_pwm_set, LV_ALIGN_CENTER, 0, 65);

    img_light_pwm_0 = lv_img_create(page);
    lv_img_set_src(img_light_pwm_0, &light_close_status);
    lv_obj_add_flag(img_light_pwm_0, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(img_light_pwm_0, LV_ALIGN_TOP_MID, 0, 0);

    img_light_pwm_25 = lv_img_create(page);
    lv_img_set_src(img_light_pwm_25, &light_warm_25);
    lv_obj_align(img_light_pwm_25, LV_ALIGN_TOP_MID, 0, 0);

    img_light_pwm_50 = lv_img_create(page);
    lv_img_set_src(img_light_pwm_50, &light_warm_50);
    lv_obj_align(img_light_pwm_50, LV_ALIGN_TOP_MID, 0, 0);

    img_light_pwm_75 = lv_img_create(page);
    lv_img_set_src(img_light_pwm_75, &light_warm_75);
    lv_obj_add_flag(img_light_pwm_75, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(img_light_pwm_75, LV_ALIGN_TOP_MID, 0, 0);

    img_light_pwm_100 = lv_img_create(page);
    lv_img_set_src(img_light_pwm_100, &light_warm_100);
    lv_obj_add_flag(img_light_pwm_100, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(img_light_pwm_100, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_add_event_cb(page, light_2color_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(page, light_2color_event_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(page, light_2color_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(page, light_2color_event_cb, LV_EVENT_CLICKED, NULL);
    ui_add_obj_to_encoder_group(page);

    xTaskCreate(play_audio_task, "Play Audio Task", 4 * 1024, NULL, 1, &task1_handle);
}

static bool light_2color_layer_enter_cb(void *layer)
{
    bool ret = false;

    LV_LOG_USER("");
    lv_layer_t *create_layer = layer;
    if (NULL == create_layer->lv_obj_layer)
    {
        ret = true;
        create_layer->lv_obj_layer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(create_layer->lv_obj_layer);
        lv_obj_set_size(create_layer->lv_obj_layer, LV_HOR_RES, LV_VER_RES);

        ui_light_2color_init(create_layer->lv_obj_layer);
        set_time_out(&time_20ms, 20);
        set_time_out(&time_500ms, 200);
    }

    return ret;
}

static bool light_2color_layer_exit_cb(void *layer)
{
    LV_LOG_USER("");
    bsp_led_rgb_set(0x00, 0x00, 0x00);
    // Delete everything on exit
    vTaskDelete(task1_handle);
    vQueueDelete(playback_queue);
    vSemaphoreDelete(playback_semaphore);
    vSemaphoreDelete(light_update_semaphore);
    return true;
}

static void light_2color_layer_timer_cb(lv_timer_t *tmr)
{
    uint32_t RGB_color = 0xFF;

    feed_clock_time();
    // Attempt to take the semaphore without blocking
    if (xSemaphoreTake(light_update_semaphore, 0) == pdFALSE)
    {
        return;
    }

    if ((light_set_conf.light_pwm ^ light_xor.light_pwm) || (light_set_conf.light_cck ^ light_xor.light_cck))
    {
        light_xor.light_pwm = light_set_conf.light_pwm;
        light_xor.light_cck = light_set_conf.light_cck;

        if (LIGHT_CCK_COOL == light_xor.light_cck)
        {
            RGB_color = (0xFF * light_xor.light_pwm / 100) << 16 | (0xFF * light_xor.light_pwm / 100) << 8 | (0xFF * light_xor.light_pwm / 100) << 0;
        }
        else
        {
            RGB_color = (0xFF * light_xor.light_pwm / 100) << 16 | (0xFF * light_xor.light_pwm / 100) << 8 | (0x33 * light_xor.light_pwm / 100) << 0;
        }
        bsp_led_rgb_set((RGB_color >> 16) & 0xFF, (RGB_color >> 8) & 0xFF, (RGB_color >> 0) & 0xFF);

        lv_obj_add_flag(img_light_pwm_100, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img_light_pwm_75, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img_light_pwm_50, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img_light_pwm_25, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img_light_pwm_0, LV_OBJ_FLAG_HIDDEN);

        if (light_set_conf.light_pwm)
        {
            lv_label_set_text_fmt(label_pwm_set, "%d%%", light_set_conf.light_pwm);
        }
        else
        {
            lv_label_set_text(label_pwm_set, "--");
        }

        uint8_t cck_set = (uint8_t)light_xor.light_cck;

        switch (light_xor.light_pwm)
        {
        case 100:
            lv_obj_clear_flag(img_light_pwm_100, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(img_light_pwm_100, light_image.img_pwm_100[cck_set]);
            lv_img_set_src(img_light_bg, light_image.img_bg[cck_set]);
            break;
        case 75:
            lv_obj_clear_flag(img_light_pwm_75, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(img_light_pwm_75, light_image.img_pwm_75[cck_set]);
            lv_img_set_src(img_light_bg, light_image.img_bg[cck_set]);
            break;
        case 50:
            lv_obj_clear_flag(img_light_pwm_50, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(img_light_pwm_50, light_image.img_pwm_50[cck_set]);
            lv_img_set_src(img_light_bg, light_image.img_bg[cck_set]);
            break;
        case 25:
            lv_obj_clear_flag(img_light_pwm_25, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(img_light_pwm_25, light_image.img_pwm_25[cck_set]);
            lv_img_set_src(img_light_bg, light_image.img_bg[cck_set]);
            break;
        default:
            break;
        }
    }
}