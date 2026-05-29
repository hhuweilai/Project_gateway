/*
 * gpio_ops.h -- GPIO ops for relay/buzzer/RGB_LED (shared by gateway_app)
 *
 * Pin mapping (all GPIO2):
 *   GPIO32 (GPIO2_IO00) -> Relay
 *   GPIO33 (GPIO2_IO01) -> Buzzer
 *   GPIO34 (GPIO2_IO02) -> RGB LED Red
 *   GPIO35 (GPIO2_IO03) -> RGB LED Green
 *   GPIO36 (GPIO2_IO04) -> RGB LED Blue
 *   GPIO37 (GPIO2_IO05) -> IR obstacle sensor (input)
 */

#ifndef GPIO_OPS_H
#define GPIO_OPS_H

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_RELAY   32
#define GPIO_BUZZER  33
#define GPIO_LED_R   34
#define GPIO_LED_G   35
#define GPIO_LED_B   36
#define GPIO_IR      37

void gpio_export_all(void);

void gpio_write(int gpio, int value);
int  gpio_read(int gpio);

void relay_on(void);
void relay_off(void);
void buzzer_on(void);
void buzzer_off(void);
void rgb_led(const char *color);

/* Alarm sequence (blocks 2 seconds): red LED + buzzer + relay -> green */
void trigger_alarm(void);

#ifdef __cplusplus
}
#endif

#endif