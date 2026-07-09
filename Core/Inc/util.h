/*
 * utils.h
 */

#ifndef INC_UTIL_H_
#define INC_UTIL_H_
#include "stm32h7xx_hal.h"

void PrintBootBanner(UART_HandleTypeDef *huart, const char *version);

#endif /* INC_UTIL_H_ */
