/*
 * led_driver.h
 *
 *  Created on: Nov 22, 2024
 *      Author: GeorgeVigelette
 */

#ifndef INC_LED_DRIVER_H_
#define INC_LED_DRIVER_H_
#include "stm32h7xx_hal.h"

#define IND2_Pin GPIO_PIN_4
#define IND2_GPIO_Port GPIOD
#define IND1_Pin GPIO_PIN_3
#define IND1_GPIO_Port GPIOA
#define IND3_Pin GPIO_PIN_5
#define IND3_GPIO_Port GPIOD

// LED States
typedef enum {
    LED_OFF = 0,
    LED_ON = 1
} LED_State;

typedef enum {
    LED_NONE = 0,
    LED_RED = 1,
    LED_GREEN = 2,
    LED_BLUE = 3
} LED_COLORS;

// Function prototypes
void LED_Init(void);
void LED_SetState(GPIO_TypeDef *GPIO_Port, uint16_t GPIO_Pin, LED_State state);
void LED_Toggle(GPIO_TypeDef *GPIO_Port, uint16_t GPIO_Pin);
void LED_RGB_SET(uint8_t state);
uint8_t LED_RGB_GET(void);

#endif /* INC_LED_DRIVER_H_ */
