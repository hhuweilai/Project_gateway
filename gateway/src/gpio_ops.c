/*
 * gpio_ops.c ¡ª GPIO ¿ØÖÆÊµÏÖ
 */
#include "gpio_ops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define GPIO_BASE "/sys/class/gpio"

void gpio_write(int gpio, int value) {
    char path[64];
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_BASE, gpio);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char c = value ? '1' : '0';
        write(fd, &c, 1);
        close(fd);
    }
}

int gpio_read(int gpio) {
    char path[64], val;
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_BASE, gpio);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    read(fd, &val, 1);
    close(fd);
    return val == '1';
}

void gpio_export_all(void) {
    int gpios[] = {GPIO_RELAY, GPIO_BUZZER, GPIO_LED_R, GPIO_LED_G, GPIO_LED_B, GPIO_IR};
    for (int i = 0; i < 6; i++) {
        int fd = open(GPIO_BASE "/export", O_WRONLY);
        if (fd >= 0) {
            char s[4]; int n = snprintf(s, sizeof(s), "%d", gpios[i]);
            write(fd, s, n); close(fd);
        }
    }
    usleep(100000);

    char path[64], *dir;
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "%s/gpio%d/direction", GPIO_BASE, gpios[i]);
        dir = (gpios[i] == GPIO_IR) ? "in" : "out";
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, dir, strlen(dir)); close(fd); }
    }
}

void relay_on(void)  { gpio_write(GPIO_RELAY, 1); }
void relay_off(void) { gpio_write(GPIO_RELAY, 0); }
void buzzer_on(void)  { gpio_write(GPIO_BUZZER, 1); }
void buzzer_off(void) { gpio_write(GPIO_BUZZER, 0); }

void rgb_led(const char *color) {
    if (strcmp(color, "red") == 0)    { gpio_write(GPIO_LED_R,1); gpio_write(GPIO_LED_G,0); gpio_write(GPIO_LED_B,0); }
    else if (strcmp(color, "green")==0){ gpio_write(GPIO_LED_R,0); gpio_write(GPIO_LED_G,1); gpio_write(GPIO_LED_B,0); }
    else if (strcmp(color, "blue")==0) { gpio_write(GPIO_LED_R,0); gpio_write(GPIO_LED_G,0); gpio_write(GPIO_LED_B,1); }
    else if (strcmp(color, "off")==0)  { gpio_write(GPIO_LED_R,0); gpio_write(GPIO_LED_G,0); gpio_write(GPIO_LED_B,0); }
    else if (strcmp(color, "white")==0){ gpio_write(GPIO_LED_R,1); gpio_write(GPIO_LED_G,1); gpio_write(GPIO_LED_B,1); }
    printf("[GPIO] LED: %s\n", color);
}

void trigger_alarm(void) {
    printf("[GPIO] === ALARM ===\n");
    rgb_led("red");
    buzzer_on();
    relay_on();
    usleep(2000000);
    buzzer_off();
    relay_off();
    rgb_led("green");
    printf("[GPIO] === CLEAR ===\n");
}