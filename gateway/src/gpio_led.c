/*
 * gpio_led.c -- control i.MX6ULL sys-led via /sys/class/leds
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define LED_TRIGGER    "/sys/class/leds/sys-led/trigger"
#define LED_BRIGHTNESS "/sys/class/leds/sys-led/brightness"

static int led_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    write(fd, value, strlen(value));
    close(fd);
    return 0;
}

int main(void)
{
    printf("=== GPIO LED Test (sys-led) ===\n");

    led_write(LED_TRIGGER, "none");

    for (int i = 0; i < 5; i++) {
        led_write(LED_BRIGHTNESS, "1");
        usleep(300000);
        led_write(LED_BRIGHTNESS, "0");
        usleep(300000);
    }

    printf("Done!\n");
    return 0;
}