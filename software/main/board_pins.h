#pragma once

#include <cstdint>

#include "driver/gpio.h"

static constexpr gpio_num_t PIN_SPARE_GPIO0 = GPIO_NUM_0;
static constexpr gpio_num_t PIN_ARM_KEY = GPIO_NUM_1;

static constexpr gpio_num_t PIN_MPU_INT = GPIO_NUM_2;
static constexpr gpio_num_t PIN_MPU_SCL = GPIO_NUM_3;
static constexpr gpio_num_t PIN_MPU_SDA = GPIO_NUM_4;

static constexpr gpio_num_t PIN_DRV_SLEEP = GPIO_NUM_5;
// Per schematic: DRV8837 signals connected to these MCU pins
static constexpr gpio_num_t PIN_DRV_IN1 = GPIO_NUM_6;
static constexpr gpio_num_t PIN_DRV_IN2 = GPIO_NUM_7;

static constexpr gpio_num_t PIN_STATUS_LED = GPIO_NUM_8;
static constexpr bool STATUS_LED_ACTIVE_LOW = true;

static constexpr gpio_num_t PIN_OLED_SDA = GPIO_NUM_9;
static constexpr gpio_num_t PIN_OLED_SCL = GPIO_NUM_10;

static constexpr gpio_num_t PIN_UART_RX = GPIO_NUM_20;
static constexpr gpio_num_t PIN_UART_TX = GPIO_NUM_21;

static constexpr uint8_t MPU6050_I2C_ADDR = 0x68;
static constexpr uint8_t OLED_I2C_ADDR = 0x3C;
