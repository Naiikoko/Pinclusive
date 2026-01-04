/*
 * Copyright (c) 2025, Nicolas FIERS, based on Blueretro (Jacques Gagnon)
 * SPDX-License-Identifier: Apache-2.0
 */

    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>
    #include <soc/efuse_reg.h>
    #include "zephyr/atomic.h"
    #include "system/gpio.h"
    #include "driver/ledc.h"
    #include "adapter/config.h"
    #include "led.h"
    // On inclut adapter.h pour avoir BT_MAX_DEV
    #include "adapter/adapter.h"

    // --- AJOUTS ET DECLARATIONS ---
    // Prototype de la tâche (nécessaire pour que err_led_init la connaisse avant sa définition)
    static void led_notify_task(void *param);

    // Lien vers la fonction externe définie dans adapter.c
    extern bool adapter_is_device_connected_and_unmuted(uint8_t dev_id);

    static TaskHandle_t led_notify_task_hdl = NULL;
    // ------------------------------

    #ifdef CONFIG_BLUERETRO_SYSTEM_SEA_BOARD
    #define ERR_LED_PIN 32
    #else
    #define ERR_LED_PIN 17
    #endif
    #define PICO_ERR_LED_PIN 20

    /* LED flags */
    enum {
        ERR_LED_SET = 0,
    };

    static atomic_t led_flags = 0;
    static TaskHandle_t err_led_task_hdl;
    static uint8_t err_led_pin = ERR_LED_PIN;

    static void err_led_task(void *param) {
        while (1) {
            if (!atomic_test_bit(&led_flags, ERR_LED_SET)) {
                ledc_set_fade_time_and_start(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_duty_max,
                    hw_config.led_pulse_fade_time_ms, LEDC_FADE_NO_WAIT);
                vTaskDelay(hw_config.led_pulse_fade_cycle_delay_ms / portTICK_PERIOD_MS);
                ledc_set_fade_time_and_start(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_duty_min,
                    hw_config.led_pulse_fade_time_ms, LEDC_FADE_NO_WAIT);
                vTaskDelay(hw_config.led_pulse_fade_cycle_delay_ms / portTICK_PERIOD_MS);
            }
            else {
                vTaskSuspend(err_led_task_hdl);
            }
        }
    }

    void err_led_init(uint32_t package) {
        // Initialisation des structures LEDC (C'est ici que vous aviez l'erreur de variable undeclared)
        ledc_timer_config_t ledc_timer = {
            .duty_resolution = LEDC_TIMER_13_BIT,
            .freq_hz = hw_config.led_pulse_hz,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .timer_num = LEDC_TIMER_0,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_channel_config_t ledc_channel = {
            .channel    = LEDC_CHANNEL_0,
            .duty       = hw_config.led_pulse_off_duty_cycle,
            .gpio_num   = ERR_LED_PIN,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER_0,
        };

        if (package == EFUSE_RD_CHIP_VER_PKG_ESP32PICOV302) {
            ledc_channel.gpio_num = PICO_ERR_LED_PIN;
            err_led_pin = PICO_ERR_LED_PIN;
        }

        ledc_timer_config(&ledc_timer);
        ledc_channel_config(&ledc_channel);
        ledc_fade_func_install(0);
        ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_off_duty_cycle, 0);

        // Démarrage de la tâche LED standard
        xTaskCreatePinnedToCore(&err_led_task, "err_led_task", 768, NULL, 5, &err_led_task_hdl, 0);
        err_led_clear();

        // Démarrage de VOTRE tâche de notification (Mute)
        // On utilise la déclaration faite tout en haut du fichier
        xTaskCreatePinnedToCore(&led_notify_task, "led_notify_task", 1536, NULL, 4, &led_notify_task_hdl, 0);
    }

    void err_led_cfg_update(void) {
        ledc_set_freq(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_0, hw_config.led_pulse_hz);
    }

    void err_led_set(void) {
        vTaskSuspend(err_led_task_hdl);
        ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_on_duty_cycle, 0);
        atomic_set_bit(&led_flags, ERR_LED_SET);
    }

    void err_led_clear(void) {
        /* When error is set it stay on until power cycle */
        if (!atomic_test_bit(&led_flags, ERR_LED_SET)) {
            vTaskSuspend(err_led_task_hdl);
            ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_off_duty_cycle, 0);
        }
    }

    void err_led_pulse(void) {
        vTaskResume(err_led_task_hdl);
    }

    uint32_t err_led_get_pin(void) {
        return err_led_pin;
    }

    // --- AJOUT DE LA TACHE DE NOTIFICATION ---
    static void led_notify_task(void *param) {
        const int flash_on_ms = 100;    
        const int flash_off_ms = 200;   
        const int pause_dev_ms = 500;   
        const int cycle_delay_ms = 2000; 

        while (1) {
            bool active_devices_found = false;

            // Scan des devices connectés et non-mutés
            for (int i = 0; i < BT_MAX_DEV; i++) {
                if (adapter_is_device_connected_and_unmuted(i)) {
                    active_devices_found = true;
                    break;
                }
            }

            if (!active_devices_found) {
                // Personne à signaler, on attend
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                continue; 
            }

            // --- Début de la séquence de clignotement ---

            // 1. On suspend la tâche standard pour éviter les conflits PWM
            if (err_led_task_hdl != NULL) {
                vTaskSuspend(err_led_task_hdl);
            }

            // 2. On force l'extinction avant de commencer
            ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_off_duty_cycle, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);

            // 3. Boucle sur chaque device
            for (int dev = 0; dev < BT_MAX_DEV; dev++) {
                if (adapter_is_device_connected_and_unmuted(dev)) {
                    
                    // Nombre de flashs = ID + 1
                    int blinks = dev + 1;

                    for (int b = 0; b < blinks; b++) {
                        // ON
                        ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_on_duty_cycle, 0);
                        vTaskDelay(flash_on_ms / portTICK_PERIOD_MS);

                        // OFF
                        ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, hw_config.led_pulse_off_duty_cycle, 0);
                        vTaskDelay(flash_off_ms / portTICK_PERIOD_MS);
                    }

                    // Pause entre les devices
                    vTaskDelay(pause_dev_ms / portTICK_PERIOD_MS);
                }
            }

            // 4. Pause longue avant de recommencer le cycle complet
            vTaskDelay(cycle_delay_ms / portTICK_PERIOD_MS);
        }
    }
