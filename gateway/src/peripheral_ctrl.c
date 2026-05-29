/*
 * peripheral_ctrl.c -- unified GPIO control for relay/buzzer/RGB_LED/IR
 *
 * Pin mapping (all GPIO2):
 *   GPIO32 (GPIO2_IO00) -> Relay
 *   GPIO33 (GPIO2_IO01) -> Buzzer
 *   GPIO34 (GPIO2_IO02) -> RGB LED Red
 *   GPIO35 (GPIO2_IO03) -> RGB LED Green
 *   GPIO36 (GPIO2_IO04) -> RGB LED Blue
 *   GPIO37 (GPIO2_IO05) -> IR obstacle sensor (input)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define GPIO_BASE   "/sys/class/gpio"
#define RELAY       32
#define BUZZER      33
#define LED_R       34
#define LED_G       35
#define LED_B       36
#define IR_SENSOR   37

static void gpio_write(int gpio, int value) {
    char path[64];
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_BASE, gpio);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char c = value ? '1' : '0';
        write(fd, &c, 1);
        close(fd);
    }
}

static int gpio_read(int gpio) {
    char path[64], val;
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_BASE, gpio);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    read(fd, &val, 1);
    close(fd);
    return val == '1';
}

static void gpio_export_all(void) {
    int gpios[] = {RELAY, BUZZER, LED_R, LED_G, LED_B, IR_SENSOR};
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
        dir = (gpios[i] == IR_SENSOR) ? "in" : "out";
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, dir, strlen(dir)); close(fd); }
    }
}

static void relay(int on)  { gpio_write(RELAY, on);  printf("Relay: %s\n",  on ? "ON" : "OFF"); }
static void buzzer(int on) { gpio_write(BUZZER, on); printf("Buzzer: %s\n", on ? "ON" : "OFF"); }

static void rgb_led(const char *color) {
    if (strcmp(color, "red") == 0)    { gpio_write(LED_R,1); gpio_write(LED_G,0); gpio_write(LED_B,0); }
    else if (strcmp(color, "green")==0){ gpio_write(LED_R,0); gpio_write(LED_G,1); gpio_write(LED_B,0); }
    else if (strcmp(color, "blue")==0) { gpio_write(LED_R,0); gpio_write(LED_G,0); gpio_write(LED_B,1); }
    else if (strcmp(color, "off")==0)  { gpio_write(LED_R,0); gpio_write(LED_G,0); gpio_write(LED_B,0); }
    else if (strcmp(color, "white")==0){ gpio_write(LED_R,1); gpio_write(LED_G,1); gpio_write(LED_B,1); }
    printf("RGB LED: %s\n", color);
}

static void trigger_alarm(void) {
    printf("=== ALARM TRIGGERED ===\n");
    rgb_led("red");
    buzzer(1);
    relay(1);
    usleep(2000000);
    buzzer(0);
    relay(0);
    rgb_led("green");
    printf("=== ALARM CLEARED ===\n");
}

int main(int argc, char *argv[]) {
    gpio_export_all();

    if (argc < 2) {
        printf("Usage: %s <cmd>\n", argv[0]);
        printf("  relay on|off\n");
        printf("  buzzer on|off\n");
        printf("  led red|green|blue|white|off\n");
        printf("  ir           (read obstacle sensor)\n");
        printf("  alarm        (demo: red + buzzer + relay -> green)\n");
        printf("  demo         (full demo sequence)\n");
        return 1;
    }

    if (strcmp(argv[1], "relay") == 0 && argc == 3)
        relay(strcmp(argv[2], "on") == 0);
    else if (strcmp(argv[1], "buzzer") == 0 && argc == 3)
        buzzer(strcmp(argv[2], "on") == 0);
    else if (strcmp(argv[1], "led") == 0 && argc == 3)
        rgb_led(argv[2]);
    else if (strcmp(argv[1], "ir") == 0)
        printf("IR Sensor: %d\n", gpio_read(IR_SENSOR));
    else if (strcmp(argv[1], "alarm") == 0)
        trigger_alarm();
    else if (strcmp(argv[1], "demo") == 0) {
        printf("LED: red -> green -> blue -> white -> off\n");
        rgb_led("red");   usleep(500000);
        rgb_led("green"); usleep(500000);
        rgb_led("blue");  usleep(500000);
        rgb_led("white"); usleep(500000);
        rgb_led("off");
        printf("Buzzer: ON -> OFF\n");
        buzzer(1); usleep(500000); buzzer(0);
        printf("Relay: ON -> OFF\n");
        relay(1); usleep(500000); relay(0);
        printf("IR: %d\n", gpio_read(IR_SENSOR));
        printf("\n--- Full alarm sequence ---\n");
        trigger_alarm();
    } else {
        printf("Unknown command\n");
        return 1;
    }
    return 0;
}