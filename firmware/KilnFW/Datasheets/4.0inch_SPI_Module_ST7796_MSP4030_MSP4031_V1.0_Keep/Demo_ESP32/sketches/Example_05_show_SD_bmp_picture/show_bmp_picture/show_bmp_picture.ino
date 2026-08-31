/***********************************************************************************
*This program is a demo of how to display a bmp picture from SD card
*This demo was made for LCD modules with 8bit or 16bit data port.
*This program requires the the LCDKIWI library.

* File                : show_bmp_picture.ino
* Hardware Environment: Arduino UNO
* Build Environment   : Arduino

*Set the pins to the correct ones for your development shield or breakout board.
*This demo use the BREAKOUT BOARD only and use these 8bit data lines to the LCD,
*pin usage as follow:
//                   CS  DC/RS  RESET  SDI/MOSI  SCK  SDO/MISO  LED  SD_CS   VCC   GND    
//ESP32-WROOM-32E:   15    2      27      13     14      12     21     22    5V    GND  

*Remember to set the pins to suit your display module!
*
* @attention
*
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
* TIME. AS A RESULT, QD electronic SHALL NOT BE HELD LIABLE FOR ANY
* DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
* FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE 
* CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
**********************************************************************************/
#include <TFT_eSPI.h> 
#include <SPI.h>
#include "SD.h"

#define SD_CS 22

TFT_eSPI mylcd = TFT_eSPI(); 
SPIClass MySPI(HSPI);

#define  BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

#define PIXEL_NUMBER  (mylcd.width()/4)
#define FILE_NUMBER 4
#define FILE_NAME_SIZE_MAX 20

uint32_t bmp_offset = 0;
uint16_t s_width = mylcd.width();  
uint16_t s_heigh = mylcd.height();
//int16_t PIXEL_NUMBER;

char file_name[FILE_NUMBER][FILE_NAME_SIZE_MAX];

uint16_t read_16(File fp)
{
    uint8_t low;
    uint16_t high;
    low = fp.read();
    high = fp.read();
    return (high<<8)|low;
}

uint32_t read_32(File fp)
{
    uint16_t low;
    uint32_t high;
    low = read_16(fp);
    high = read_16(fp);
    return (high<<8)|low;   
 }
 
bool analysis_bpm_header(File fp)
{
    if(read_16(fp) != 0x4D42)
    {
      return false;  
    }
    //get bpm size
    read_32(fp);
    //get creator information
    read_32(fp);
    //get offset information
    bmp_offset = read_32(fp);
    //get DIB infomation
    read_32(fp);
    //get width and heigh information
    uint32_t bpm_width = read_32(fp);
    uint32_t bpm_heigh = read_32(fp);
    if((bpm_width != s_width) || (bpm_heigh != s_heigh))
    {
      return false; 
    }
    if(read_16(fp) != 1)
    {
        return false;
    }
    read_16(fp);
    if(read_32(fp) != 0)
    {
      return false; 
     }
     return true;
}

void draw_bmp_picture(File fp)
{
  uint16_t i,j,k,l,m=0;
  uint8_t bpm_data[PIXEL_NUMBER*3] = {0};
  uint16_t bpm_color[PIXEL_NUMBER];
  fp.seek(bmp_offset);
  for(i = 0;i < s_heigh;i++)
  {
    for(j = 0;j<s_width/PIXEL_NUMBER;j++)
    {
      m = 0;
      fp.read(bpm_data,PIXEL_NUMBER*3);
      for(k = 0;k<PIXEL_NUMBER;k++)
      {
        bpm_color[k]= mylcd.color565(bpm_data[m+2], bpm_data[m+1], bpm_data[m+0]); //change to 565
        m +=3;
      }
      for(l = 0;l<PIXEL_NUMBER;l++)
      {
        mylcd.drawPixel(j*PIXEL_NUMBER+l,i, bpm_color[l]);
      }    
     }
   }    
}

void setup() 
{   
   mylcd.init();
   mylcd.fillScreen(BLACK);
   if(PIXEL_NUMBER == 60) //240*320
   {
       strcpy(file_name[0],"/tulip.bmp");
       strcpy(file_name[1],"/game.bmp");
       strcpy(file_name[2],"/tree.bmp");
       strcpy(file_name[3],"/flower.bmp");
   }
   else //320*480
   {
       strcpy(file_name[0],"/01.bmp");
       strcpy(file_name[1],"/02.bmp");
       strcpy(file_name[2],"/03.bmp");
       strcpy(file_name[3],"/04.bmp");
   }
  //Init SD_Card
   pinMode(SD_CS, OUTPUT);
   digitalWrite(SD_CS, HIGH);
    if (!SD.begin(SD_CS,MySPI)) 
    {
      mylcd.fillScreen(BLUE);
      mylcd.setTextColor(WHITE);
      mylcd.drawString("SD Card Init fail!", 2,2,2);
      while(1);
    }
}

void loop() 
{
    int i = 0;
    File bmp_file;
    for(i = 0;i<FILE_NUMBER;i++)
    {
       bmp_file = SD.open(file_name[i]);
       if(!bmp_file)
       {
            mylcd.fillScreen(BLUE);
            mylcd.setTextColor(WHITE);
            mylcd.drawString("didnt find BMPimage!", 2,20,2);
            while(1);
        }
        if(!analysis_bpm_header(bmp_file))
        {  
            mylcd.fillScreen(BLUE);
            mylcd.setTextColor(WHITE);
            mylcd.drawString("bad bmp picture!", 2,2,2);
            return;
        }
          draw_bmp_picture(bmp_file);
         bmp_file.close(); 
         delay(2000);
     }
}
