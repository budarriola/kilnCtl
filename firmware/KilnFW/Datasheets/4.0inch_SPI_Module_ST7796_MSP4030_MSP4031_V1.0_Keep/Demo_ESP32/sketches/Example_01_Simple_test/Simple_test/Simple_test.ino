//This application does not rely on any libraries and it is for ST7796

//This program is a demo of clearing screen.

//when using the BREAKOUT BOARD only and using these hardware spi lines to the LCD,
//the SDA pin and SCK pin is defined by the system and can't be modified.
//if you don't need to control the LED pin,you can set it to 3.3V and set the pin definition to -1.
//other pins can be defined by youself,for example
//pin usage as follow:
//                   CS  DC/RS  RESET  SDI/MOSI  SCK  SDO/MISO  LED    VCC     GND    
//ESP32-WROOM-32E:   15    2      27      13     14      12     21      5V     GND                  

//Remember to set the pins to suit your display module!
/********************************************************************************
* @attention
*
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
* TIME. AS A RESULT, QD electronic SHALL NOT BE HELD LIABLE FOR ANY
* DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
* FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE 
* CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
**********************************************************************************/
#include <Arduino.h>
#include <SPI.h>
#include <Print.h>
#include "spi_dev.h"

#define RED 0xF800
#define GREEN 0x07E0
#define BLUE 0x001F
#define WHITE 0xFFFF
#define BLACK 0x0

#define LCD_WIDTH 320
#define LCD_HEIGHT 480

#define WR_RAM_CMD 0x2C
#define RD_RAM_CMD 0x2E
#define SET_X_CMD  0x2A
#define SET_Y_CMD  0x2B
#define MADCTL_CMD 0x36

bool lock_flag = true;

SPIClass spi = SPIClass(SPI_PORT);

void SPI_Start_Write(void)
{
    if(lock_flag)
    {
       lock_flag = false;
       spi.beginTransaction(SPISettings(SPI_FREQUENCY, MSBFIRST, SPI_MODE));
       LCD_CS_LOW;
       SET_SPI_WRITE_MODE;
    }
}

void SPI_End_Write(void)
{
    if(!lock_flag)
    {
      lock_flag = true;
      LCD_CS_HIGH;
      SET_SPI_READ_MODE;
      spi.endTransaction();
    }
}

void LCD_Write_Reg(uint8_t val)
{
    SPI_Start_Write(); 
    LCD_DC_LOW;
    spi_write_8bit(val);
    SPI_End_Write();
}

void LCD_Write_Data_8Bit(uint8_t val)
{
    SPI_Start_Write();  
    LCD_DC_HIGH;
    spi_write_8bit(val);
    SPI_End_Write();
}

void LCD_Write_Data_16Bit(uint16_t val)
{
    SPI_Start_Write(); 
    LCD_DC_HIGH;
    spi_write_16bit(val);
    SPI_End_Write();
}

void LCD_Set_Windows(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey)
{
    LCD_Write_Reg(SET_X_CMD);
    LCD_Write_Data_16Bit(sx);
    LCD_Write_Data_16Bit(ex);
    LCD_Write_Reg(SET_Y_CMD);
    LCD_Write_Data_16Bit(sy);
    LCD_Write_Data_16Bit(ey);
    LCD_Write_Reg(WR_RAM_CMD);
}

void SPI_Init(void)
{
    spi.begin(SPI_SCLK, SPI_MISO, SPI_MOSI, -1);   
}

void Lcd_Init(void)
{
    LCD_RST_LOW;
    delay(20);
    LCD_RST_HIGH;
    delay(20);
  LCD_Write_Reg(0xF0);
  LCD_Write_Data_8Bit(0xC3);
  LCD_Write_Reg(0xF0);
  LCD_Write_Data_8Bit(0x96);
  LCD_Write_Reg(0x36);
  LCD_Write_Data_8Bit(0x48);  
  LCD_Write_Reg(0x3A);
  LCD_Write_Data_8Bit(0x05);  
  LCD_Write_Reg(0xB0);
  LCD_Write_Data_8Bit(0x80);  
  LCD_Write_Reg(0xB6);
  LCD_Write_Data_8Bit(0x00);
  LCD_Write_Data_8Bit(0x02);  
  LCD_Write_Reg(0xB5);
  LCD_Write_Data_8Bit(0x02);
  LCD_Write_Data_8Bit(0x03);
  LCD_Write_Data_8Bit(0x00);
  LCD_Write_Data_8Bit(0x04);
  LCD_Write_Reg(0xB1);
  LCD_Write_Data_8Bit(0x80);  
  LCD_Write_Data_8Bit(0x10);  
  LCD_Write_Reg(0xB4);
  LCD_Write_Data_8Bit(0x00);
  LCD_Write_Reg(0xB7);
  LCD_Write_Data_8Bit(0xC6);
  LCD_Write_Reg(0xC5);
  LCD_Write_Data_8Bit(0x1C);
  LCD_Write_Reg(0xE4);
  LCD_Write_Data_8Bit(0x31);
  LCD_Write_Reg(0xE8);
  LCD_Write_Data_8Bit(0x40);
  LCD_Write_Data_8Bit(0x8A);
  LCD_Write_Data_8Bit(0x00);
  LCD_Write_Data_8Bit(0x00);
  LCD_Write_Data_8Bit(0x29);
  LCD_Write_Data_8Bit(0x19);
  LCD_Write_Data_8Bit(0xA5);
  LCD_Write_Data_8Bit(0x33);
  LCD_Write_Reg(0xC2);
  LCD_Write_Reg(0xA7);
  
  LCD_Write_Reg(0xE0);
  LCD_Write_Data_8Bit(0xF0);
  LCD_Write_Data_8Bit(0x09);
  LCD_Write_Data_8Bit(0x13);
  LCD_Write_Data_8Bit(0x12);
  LCD_Write_Data_8Bit(0x12);
  LCD_Write_Data_8Bit(0x2B);
  LCD_Write_Data_8Bit(0x3C);
  LCD_Write_Data_8Bit(0x44);
  LCD_Write_Data_8Bit(0x4B);
  LCD_Write_Data_8Bit(0x1B);
  LCD_Write_Data_8Bit(0x18);
  LCD_Write_Data_8Bit(0x17);
  LCD_Write_Data_8Bit(0x1D);
  LCD_Write_Data_8Bit(0x21);

  LCD_Write_Reg(0XE1);
  LCD_Write_Data_8Bit(0xF0);
  LCD_Write_Data_8Bit(0x09);
  LCD_Write_Data_8Bit(0x13);
  LCD_Write_Data_8Bit(0x0C);
  LCD_Write_Data_8Bit(0x0D);
  LCD_Write_Data_8Bit(0x27);
  LCD_Write_Data_8Bit(0x3B);
  LCD_Write_Data_8Bit(0x44);
  LCD_Write_Data_8Bit(0x4D);
  LCD_Write_Data_8Bit(0x0B);
  LCD_Write_Data_8Bit(0x17);
  LCD_Write_Data_8Bit(0x17);
  LCD_Write_Data_8Bit(0x1D);
  LCD_Write_Data_8Bit(0x21);

  LCD_Write_Reg(0xF0);
  LCD_Write_Data_8Bit(0x3C);
  LCD_Write_Reg(0xF0);
  LCD_Write_Data_8Bit(0x69);
  LCD_Write_Reg(0X13);
  LCD_Write_Reg(0X11);
  LCD_Write_Reg(0X29);

}

void Write_color_Block(uint16_t color, uint32_t len)
{
    volatile uint32_t* wr_buf = spi_write_buf;
    uint32_t color32 = ((color << 8 | color >> 8) << 16) | (color << 8 | color >> 8);
    uint8_t num = len%32,i=0;
    if(num)
    {
        while(*spi_cmd & SPI_USR);
        while(i < num)
        {
            *wr_buf++ = color32;
            i += 2; 
        }
        *spi_write_len = num * 16 - 1;
        *spi_cmd = SPI_USR;
        if(len < 32)
        {
            return;
        }
        len -= num;
        for(i /= 2; i < 16; i++)
        {
            *wr_buf++ = color32;
        }
    }
    while(*spi_cmd & SPI_USR);
    if(!num)
    {    
        while(i < 16)
        {
            *wr_buf++ = color32;
            i++; 
        }
    }
    *spi_write_len = 511;
    while(len)
    {
        while(*spi_cmd & SPI_USR);
        *spi_cmd = SPI_USR;
        len -= 32;
    }
    while(*spi_cmd & SPI_USR);
}

void LCD_Clear_Screen(uint16_t sx, uint16_t sy, uint16_t w, uint16_t h, uint16_t color)
{
    if((sx >= LCD_WIDTH) || (sy >= LCD_HEIGHT))
        return;
    if(((sx + w) > LCD_WIDTH) || ((sy + h) > LCD_HEIGHT))
        return;
    if(((w < 1) || (w > LCD_WIDTH)) || ((h < 1) || (h > LCD_HEIGHT)))
        return;
    LCD_Set_Windows(sx, sy, sx + w - 1, sy + h -1);
    LCD_DC_HIGH;
    SPI_Start_Write(); 
    Write_color_Block(color, w*h);
    SPI_End_Write();
}

void setup()
{
  pinMode(LCD_CS, OUTPUT);
  digitalWrite(LCD_CS, HIGH);
  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_RST, HIGH);    
  pinMode(LCD_DC, OUTPUT);
  digitalWrite(LCD_DC, HIGH);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  SPI_Init();
  Lcd_Init();
}

void loop()
{  
   LCD_Clear_Screen(0, 0, LCD_WIDTH, LCD_HEIGHT, RED);delay(500);
   LCD_Clear_Screen(0, 0, LCD_WIDTH, LCD_HEIGHT, GREEN);delay(500);
   LCD_Clear_Screen(0, 0, LCD_WIDTH, LCD_HEIGHT, BLUE);delay(500);
   LCD_Clear_Screen(0, 0, LCD_WIDTH, LCD_HEIGHT, WHITE);delay(500);
   LCD_Clear_Screen(0, 0, LCD_WIDTH, LCD_HEIGHT, BLACK);delay(500);
   for(uint16_t i=0; i< 5000;i++)
   {
      LCD_Clear_Screen(random(LCD_WIDTH-1), random(LCD_HEIGHT-1), random(LCD_WIDTH), random(LCD_HEIGHT), random(0xFFFF));
   }
}
