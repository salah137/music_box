#ifndef I2C_CONNECTION_H
#define I2C_CONNECTION_H

#include <esp_err.h>
#include <hal/i2c_types.h>
#include <driver/i2c_types.h>

/**
 * @brief  struct that contains the device handler
 */
struct {} typedef screen_t;

/** 
 * @brief initialize the i2c communication buss
*/
bool init_bus(int sda, int scl, screen_t* screen);

/**
 * @brief send SSD1306 command
 */
void ssd1306_command(uint8_t command, screen_t* screen);

/**
 * @brief send data buffer
 */
void ssd1306_data(uint8_t* data,int len, screen_t* screen);

#endif