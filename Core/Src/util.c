#include "util.h"
#include <stdio.h>
#include <string.h>

#define INNER_WIDTH 68

void PrintBootBanner(UART_HandleTypeDef *huart, const char *version)
{
    /* The fixed banner text is ~511 bytes; size with headroom for the version
     * field so snprintf never truncates (avoids -Wformat-truncation). */
    char banner[768];

    const int version_field_width =
        INNER_WIDTH - (int)strlen("                  Bootloader version: ");

    snprintf(banner, sizeof(banner),
        "\033c\r\n\r\n"
        "\r\n======================================================================"
        "\r\n=                (C) COPYRIGHT 2026 Openwater, Inc                   ="
        "\r\n=                                                                    ="
        "\r\n=              Secure Boot and Secure Firmware Update                ="
        "\r\n=                  Bootloader version: %-*s="
        "\r\n======================================================================"
        "\r\n"
        "\r\n= [SBOOT] Firmware Update Request - entering USB DFU download mode"
        "\r\n\r\n",
        version_field_width,
        version);

    HAL_UART_Transmit(huart,
                      (uint8_t *)banner,
                      strlen(banner),
                      HAL_MAX_DELAY);
}
