#ifndef DRAWING_H
#define DRAWING_H

#include <hal/i2c_types.h>
#include <esp_err.h>
#include "i2c_connection.h"

/** 
* @brief initialize the oled screen by creating the i2c communication
* sda : the sda pin, scl : the scl pin
* &screen refrence the screen instance
*/
void init(int sda, int scl, screen_t* screen);

/** 
* @brief horizontaly flip the screen
* direction : 1=>normal , 0=>mirrored
*/
esp_err_t horizontale_flip(screen_t* screen,uint8_t direction);


/** 
* @brief vartically flip the screen
* direction : 1=>normal , 0=>mirrored
*/
esp_err_t vertical_flip(screen_t* screen,uint8_t direction);

/** 
* @brief controle the brightness of the screen
* it varies between 0 and 7(ints)
*/
esp_err_t set_brightness(screen_t* screen,uint8_t brightness);

/** 
* @brief update the screen UI
*/
void ssd1306_update(screen_t* screen);


/** 
* @brief draw a pixel in position x , y
* color = 1 => white ; color = 0 => black
*/
void ssd1306_draw_pixel(int x, int y, int color);

/** 
* @brief draw a horizontal line in position x , y with lenth and thikness
*/
void ssd1306_draw_horizental_line(int x, int y, int length, int thikness,int color);
/** 
* @brief draw a vertical line in position x , y with lenth and thikness
*/
void ssd1306_draw_verticale_line(int x,int y, int length,int thikness,int color);

/**
*@brief draw an empty rectangle in x, y position with hight and width
*
*/
void ssd1306_draw_rectangle_empty(int x,int y,int hight,int width);

/** 
* clear the frame buffer in the ram(esp ram)
*
*/
void ssd1306_clear_fb(void);

void draw_char(int x, int y, char c,int scale, int color);
void draw_string(int x,int y, char* str, int scale, int color);

#endif