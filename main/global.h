#include "driver/i2c.h"

#ifndef GLOBALH_INCLUDED
#define GLOBALH_INCLUDED


#define I2C_MASTER_SCL_IO GPIO_NUM_22               /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO GPIO_NUM_21               /*!< gpio number for I2C master data  */

#define ACK_CHECK_EN 0x1                        /*!< I2C master will check ack from slave*/
#define WRITE_BIT I2C_MASTER_WRITE              /*!< I2C master write */
#define READ_BIT I2C_MASTER_READ                /*!< I2C master read */
#define I2C_MASTER_FREQ_HZ 400000
#define ACK_VAL I2C_MASTER_ACK                             /*!< I2C ack value */
#define NACK_VAL I2C_MASTER_NACK                             /*!< I2C nack value */

typedef struct
{
  uint8_t start_min;
  uint8_t start_hour;
  uint8_t stop_min;
  uint8_t stop_hour;
  uint8_t week_day;
  uint8_t active;
  uint8_t free;
  char name[8];
} timeplan_t;

typedef struct
{
  char name[8];
  uint8_t timeplan[10];
  uint8_t active;
  uint8_t free;
} programplan_t;


typedef struct
{
  uint8_t type;
  uint8_t version;
  uint32_t stamp;
  uint8_t ready;
  uint8_t online;
  char name[10];
} rs_device_t;

struct struct_remote_room_thermostat
{
  int light;
  uint8_t term_mode;
  uint8_t programs;
  uint8_t active_program;
  float term_threshold[3];
  char term_name[3][10];
  uint8_t ready ;
} remote_room_thermostat[32];

extern i2c_port_t i2c_num;

struct struct_my_device
{
  uint8_t myIP[4];
  uint8_t myMASK[4];
  uint8_t myDNS[4];
  uint8_t myGW[4];
  char nazev[8];
  char mqtt_uri[60];
  char wifi_essid[20];
  char wifi_pass[20];
  uint8_t ip_static;
} device;

#endif
