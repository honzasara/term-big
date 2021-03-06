#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <soc/rtc.h>
#include "freertos/timers.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/apps/sntp.h"
#include <sys/param.h>
#include <esp_http_server.h>
#include "cJSON.h"
#include "term-big-1.h"


#define RS_TXD  (GPIO_NUM_17) 
#define RS_RXD  (GPIO_NUM_16)
#define RS_RTS  (GPIO_NUM_4)
#define RS_CTS  (UART_PIN_NO_CHANGE)
#define RS_TASK_STACK_SIZE    (8196)
#define RS_TASK_PRIO          (5)
#define PACKET_READ_TICS        (15 / portTICK_RATE_MS)
#define RS_BUF_SIZE        (256)   //// definice maleho lokalniho rs bufferu



#define I2C_MEMORY_ADDR 0x50
#define DS1307_ADDRESS  0x68


#include "SSD1306.h"
#include "Font5x8.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "ow.h"

#include "send_mqtt.h"
#include "receive_mqtt.h"


i2c_port_t i2c_num;

EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
TaskHandle_t loopTaskHandle = NULL;

uint8_t wifi_connected = 0;
uint8_t mqtt_connected = 0;

uint8_t reload_network = 1;
 

esp_mqtt_client_handle_t mqtt_client = NULL;

uint8_t Global_HWwirenum = 0;
uint8_t Global_ds18s20num = 0;

uint8_t send_rsid = 0;
uint8_t find_rsid = 0;
uint8_t enable_send_network_status = 0;

////////////////////////////////////////////////////////////////////////
/// primitivni funkce
uint8_t bcd2bin (uint8_t val) { return val - 6 * (val >> 4); }
uint8_t bin2bcd (uint8_t val) { return val + 6 * (val / 10); }
void callback_30_sec(void* arg);
void send_network_status(void);
void function_ds18s20_unannounced_rom(uint8_t slot);
void function_ds18s20_announced_rom(uint8_t idx);
void function_ds18s20_set_offset(uint8_t slot, int offset);
void function_ds18s20_set_name(uint8_t slot, const char* name);
uint8_t rs_add_buffer(uint8_t rsid, char *cmd, char *args);
esp_err_t i2c_eeprom_readByte(uint8_t deviceAddress, uint16_t address, uint8_t *data);
esp_err_t i2c_eeprom_writeByte(uint8_t deviceAddress, uint16_t address, uint8_t data);
////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////
void new_parse_at(char *input, char *out1, char *out2)
{
  uint16_t count = 0;
  uint16_t q = 0;
  while ( (count < strlen(input)) && (input[count] != ',') && (q < MAX_TEMP_BUFFER - 1))
  {
    out1[q] = input[count];
    out1[q + 1] = 0;
    q++;
    count++;
  }
  count++;
  q = 0;
  while ((count < strlen(input)) && (q < MAX_TEMP_BUFFER - 1) )
  {
    out2[q] = input[count];
    out2[q + 1] = 0;
    q++;
    count++;
  }
  if (q == 0) out2[0] = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/// funkce odesialni
void rs_send_at(uint8_t id, char *cmd, char *args)
{
  char tmp1[MAX_TEMP_BUFFER];
  char tmp2[8];
  tmp1[0] = 0;
  tmp2[0] = 0;
  strcpy(tmp1, "at+");
  itoa(id, tmp2, 10);
  strcat(tmp1, tmp2);
  strcat(tmp1, ",");
  strcat(tmp1, cmd);
  if (strlen(args) > 0)
    {
    strcat(tmp1, ",");
    strcat(tmp1, args);
    }
  strcat(tmp1, ";");
  uart_write_bytes(UART_NUM_2, tmp1, strlen(tmp1));
}
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
/// ulozeni dat do RTC pameti
esp_err_t RTC_RAM_write(uint8_t addr, uint8_t data)
{
  esp_err_t ret;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, (8 + addr), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, data, ACK_CHECK_EN);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  return ret;
}
///
//nacteni dat z RTC pameti
esp_err_t RTC_RAM_read(uint8_t addr, uint8_t *data)
{
  esp_err_t ret;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, (8 + addr), ACK_CHECK_EN);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK)
    return ret;
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd,  (DS1307_ADDRESS << 1) | READ_BIT, ACK_CHECK_EN);
  i2c_master_read_byte(cmd, data, ACK_VAL);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  return ret;
}
///
/// nastaveni casu do RTC
esp_err_t RTC_DS1307_adjust(DateTime *dt) 
{
  printf("%d:%d:%d %d %d-%d-%d\n\r", dt->hour, dt->minute, dt->second, dt->day_week, dt->day, dt->month, dt->year);
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, 0, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->second), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->minute), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->hour), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->day_week), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->day), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->month), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, bin2bcd(dt->year-2000), ACK_CHECK_EN);
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  return ret;
}
///
/// nacteni casu z RTC
esp_err_t RTC_DS1307_now(DateTime *now) 
{
  now->second = 0;
  now->minute = 0;
  now->hour = 0;
  now->day_week = 0;
  now->day = 0;
  now->month = 0;
  now->year = 2000;

  esp_err_t ret;
  DateTime dt;
  i2c_cmd_handle_t cmd;
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, 0, ACK_CHECK_EN);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK)
    return ret;
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd,  (DS1307_ADDRESS << 1) | READ_BIT, ACK_CHECK_EN);
  i2c_master_read_byte(cmd, &dt.second, ACK_VAL);
  i2c_master_read_byte(cmd, &dt.minute, ACK_VAL);
  i2c_master_read_byte(cmd, &dt.hour, ACK_VAL);
  i2c_master_read_byte(cmd, &dt.day_week, ACK_VAL);
  i2c_master_read_byte(cmd, &dt.day, ACK_VAL);
  i2c_master_read_byte(cmd, &dt.month, ACK_VAL);
  i2c_master_read_byte(cmd, (uint8_t *)&dt.year, NACK_VAL);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  if (ret == ESP_OK)
    {
    now->second = bcd2bin(dt.second); 
    now->minute = bcd2bin(dt.minute);
    now->hour = bcd2bin(dt.hour);
    now->day_week = bcd2bin(dt.day_week);
    now->day = bcd2bin(dt.day);
    now->month = bcd2bin(dt.month);
    now->year = bcd2bin(dt.year)+2000; 
    }
  //printf("%d:%d:%d %d %d-%d-%d\n\r", now->hour, now->minute, now->second, now->day_week, now->day, now->month, now->year);
  return ret;
}

/// overeni zda RTC bezi
esp_err_t RTC_DS1307_isrunning(uint8_t *run) 
{
  esp_err_t ret;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, 0, ACK_CHECK_EN);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK)
    return ret;
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd,  (DS1307_ADDRESS << 1) | READ_BIT, ACK_CHECK_EN);
  i2c_master_read_byte(cmd, run, NACK_VAL);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  *run = (*run>>7);
  return ret;
}

/// naste z NTP serveru a ulozi do RTC
uint8_t sync_ntp_time_with_local_rtc(void)
{
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "82.113.53.41");
  sntp_setservername(1, "37.187.104.44");
  sntp_init();
  time_t now = 0;
  DateTime dt;
  struct tm timeinfo = { 0 };
  uint8_t retry = 0;
  uint8_t ret = 0;
  int retry_count = 10;
  char strftime_buf[64];
  while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) 
    {
     //printf("Waiting for system time to be set... (%d/%d)", retry, retry_count);
     vTaskDelay(2000 / portTICK_PERIOD_MS);
     time(&now);
     setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 0);
     tzset();
     localtime_r(&now, &timeinfo);
     strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
     printf("NTP time = %s\n\r", strftime_buf);
    }

  if (retry < retry_count -1) 
    {
     dt.second = timeinfo.tm_sec;
     dt.minute = timeinfo.tm_min;
     dt.hour = timeinfo.tm_hour;
     dt.day_week = timeinfo.tm_wday;
     dt.day = timeinfo.tm_mday;
     dt.month = timeinfo.tm_mon + 1;
     dt.year = timeinfo.tm_year + 1900;
     RTC_DS1307_adjust(&dt);
     printf("Save to RTC\n\r");
     ret = 1; 
    }
  else
   {  
    printf("Error sync with ntp server\n\r");
    ret = 0;
   }

  sntp_stop();

  return ret;
}



void sync_time_rs_device(void)
{
   char tmp_str[8];
   DateTime now;
   RTC_DS1307_now(&now);
   itoa(now.second, tmp_str, 10);
   rs_send_at(255, "css", tmp_str);
   itoa(now.minute, tmp_str, 10);
   rs_send_at(255, "csm", tmp_str);
   itoa(now.hour, tmp_str, 10);
   rs_send_at(255, "csh", tmp_str);
}



esp_err_t twi_init(i2c_port_t t_i2c_num)
{
#define I2C_MASTER_TX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */
  esp_err_t ret;
  
  i2c_num = t_i2c_num;
  
  i2c_config_t conf;
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = I2C_MASTER_SDA_IO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = I2C_MASTER_SCL_IO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 400000L;
  i2c_param_config(i2c_num, &conf);
  ret = i2c_driver_install(i2c_num, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
/*
httpd_uri_t set_program = {
    .uri       = "/set_timeprogram.html",
    .method    = HTTP_POST,
    .handler   = set_program_handler,
    .user_ctx  = "NULL"
};

httpd_uri_t set_rsid_name = {
    .uri       = "/set_rsid_name.html",
    .method    = HTTP_POST,
    .handler   = set_rsid_name_handler,
    .user_ctx  = "NULL"
};

httpd_uri_t get_index = {
    .uri       = "/index.html",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = "Hello World!"
};

httpd_uri_t get_rsid = {
    .uri       = "/rsid.html",
    .method    = HTTP_GET,
    .handler   = rsid_get_handler,
    .user_ctx  = "Hello World!"
};

httpd_uri_t get_thermostat = {
    .uri       = "/thermostat.html",
    .method    = HTTP_GET,
    .handler   = thermostat_get_handler,
    .user_ctx  = "NULL"
};

httpd_uri_t get_timeprogram = {
    .uri       = "/timeprogram.html",
    .method    = HTTP_GET,
    .handler   = timeprogram_get_handler,
    .user_ctx  = "NULL"
};

httpd_uri_t get_newprogram = {
    .uri       = "/newp.html",
    .method    = HTTP_GET,
    .handler   = new_program_handler,
    .user_ctx  = "NULL"
};
*/


httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8196*2;
    if (httpd_start(&server, &config) == ESP_OK) 
      {
        /*
        httpd_register_uri_handler(server, &get_index);
        httpd_register_uri_handler(server, &get_rsid);
        httpd_register_uri_handler(server, &set_rsid_name);
        httpd_register_uri_handler(server, &get_thermostat);
        httpd_register_uri_handler(server, &get_timeprogram);
        httpd_register_uri_handler(server, &get_newprogram);	
        httpd_register_uri_handler(server, &set_program);	
	*/
        return server;
      }
    return NULL;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
void stop_webserver(httpd_handle_t server)
{
  httpd_stop(server);
}



esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    httpd_handle_t *server = (httpd_handle_t *) ctx;
    //printf("wifi event: %d \n\r", event->event_id);
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
	    {
             esp_wifi_connect();
             break;
	    }
	case SYSTEM_EVENT_STA_CONNECTED:
	     //printf("channel: %d \n\r", event->event_info.connected.channel);
	     break;
        case SYSTEM_EVENT_STA_GOT_IP:
	    {
             xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
	     wifi_connected = 1;
             if (*server == NULL) 
	      {
	       *server = start_webserver();
              }
             break;
	    }
        case SYSTEM_EVENT_STA_DISCONNECTED:
	    {
	     wifi_connected = 0;
	     //printf("reason %d\n\r", event->event_info.disconnected.reason);
              esp_wifi_connect();
              xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
	     if (*server) 
	       {
	       stop_webserver(*server);
	       *server = NULL;
               }
	     //printf("\n\r %s jep \n\r", " - ");
             break;
	    }
        default:
            break;
    }
    return ESP_OK;
}

/////////////////////////////////////////////////////////////////////
void procces_mqtt_json(char *topic, uint8_t topic_len, char *data,  uint8_t data_len)
{
  uint8_t len, let;
  programplan_t pp;
  timeplan_t tp;
  output_t ot;
  actions_t at;
  uint8_t change = 0;
  char p_tmp1[64];
  char parse_topic[MAX_TEMP_BUFFER];
  char parse_data[MAX_TEMP_BUFFER];
  uint8_t valid_json = 1; 
  uint8_t j;
  /// funkce pro nastavovani modulu
  //
  for (uint8_t i = 0; i < topic_len; i++)
    {
    parse_topic[i] = topic[i];
    parse_topic[i + 1] = 0;
    }
  for (uint8_t i = 0; i < data_len; i++)
    {
    parse_data[i] = data[i];
    parse_data[i + 1] = 0;
    }
  ////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////
  //// funkce pro globalni setup
  p_tmp1[0] = 0;
  strcpy(p_tmp1, "/regulatory/global/");
  len = strlen(p_tmp1);
  let = topic_len;
  //printf("%s - %s , %d\n\r", parse_topic, tmp1, len);
  if (strncmp(parse_topic, p_tmp1, len) == 0)
    {
    cJSON *json = cJSON_Parse(parse_data);
    if (json == NULL)
      {
      valid_json = 0;
      //printf("Chyba cJSON: %s\n\r", parse_data);
      }
    else
      valid_json = 1;
    ///
    p_tmp1[0] = 0;
    j = 0;
    for (uint8_t p = len; p < let; p++)
      {
      p_tmp1[j] = topic[p];
      p_tmp1[j + 1] = 0;
      j++;
      }
    /// automaticky sync z ntp serveru a syncnuti vsech rsid
    if (strcmp(p_tmp1, "sync/time") == 0)
      {
      if (sync_ntp_time_with_local_rtc() == 1)
	/// pokud se nepodari syncnout cas, tak nesynchronizuji v rsid, ocekava se funkcni baterky v rtc
        sync_time_rs_device();
      }
    /// syncnu pouze cas do rsid
    if (strcmp(p_tmp1, "sync/time/rs") == 0)
      {
      sync_time_rs_device();
      }
    /// syncnu si cas z ntp serveru
    if (strcmp(p_tmp1, "sync/time/ntp") == 0)
      {
      sync_ntp_time_with_local_rtc();
      }
    //// nastaveni casu
    if ((strcmp(p_tmp1, "set/time") == 0) && (valid_json == 1))
      {
      cJSON *jhod = NULL;
      jhod = cJSON_GetObjectItemCaseSensitive(json, "hour");
      cJSON *jmin = NULL;
      jmin = cJSON_GetObjectItemCaseSensitive(json, "min");
      cJSON *jsec = NULL;
      jsec = cJSON_GetObjectItemCaseSensitive(json, "sec");
      cJSON *jweek = NULL;
      jweek = cJSON_GetObjectItemCaseSensitive(json, "dayweek");
      cJSON *jday = NULL;
      jday = cJSON_GetObjectItemCaseSensitive(json, "day");
      cJSON *jmonth = NULL;
      jhod = cJSON_GetObjectItemCaseSensitive(json, "month");
      cJSON *jyear = NULL;
      jyear = cJSON_GetObjectItemCaseSensitive(json, "year");
      DateTime now;
      RTC_DS1307_now(&now);
      if (cJSON_IsNumber(jhod)) now.hour = jhod->valueint;
      if (cJSON_IsNumber(jmin)) now.minute = jmin->valueint;
      if (cJSON_IsNumber(jsec)) now.hour = jsec->valueint;
      if (cJSON_IsNumber(jweek)) now.day_week = jweek->valueint; 
      if (cJSON_IsNumber(jday)) now.day = jday->valueint;
      if (cJSON_IsNumber(jmonth)) now.month = jmonth->valueint;
      if (cJSON_IsNumber(jyear)) now.year = jyear->valueint;
      RTC_DS1307_adjust(&now);    
      }
    ///
    cJSON_Delete(json);
    }
  //////
  /// funkce termostatu
  p_tmp1[0] = 0;
  sprintf(p_tmp1, "/regulatory/%s/thermostat/", device.nazev);
  len = strlen(p_tmp1);
  let = topic_len;
  if (strncmp(parse_topic, p_tmp1, len) == 0)
    {
    cJSON *json = cJSON_Parse(parse_data);
    if (json == NULL)
      {
      valid_json = 0;
      }
    else
      {
      valid_json = 1;
      }
    p_tmp1[0] = 0;
    j = 0;
    for (uint8_t p = len; p < let; p++)
      {
      p_tmp1[j] = topic[p];
      p_tmp1[j + 1] = 0;
      j++;
      }
    //// nastaveni pro rsid thermostat
    if ((strcmp(p_tmp1, "set") == 0) && (valid_json == 1))
      {
      cJSON *rrsid = NULL;
      rrsid = cJSON_GetObjectItemCaseSensitive(json, "rsid");
      cJSON *term_mode = NULL;
      term_mode = cJSON_GetObjectItemCaseSensitive(json, "term_mode");
      cJSON *nazev = NULL;
      nazev = cJSON_GetObjectItemCaseSensitive(json, "device_name");
      cJSON *light = NULL;
      light = cJSON_GetObjectItemCaseSensitive(json, "light");
      if (cJSON_IsNumber(rrsid))
        {
	/// nastavi intenzitu podsvetleni set light
	if (cJSON_IsNumber(light))
	  {
	  char tmp4[11];
	  sprintf(tmp4, "%d", light->valueint);
	  rs_add_buffer(rrsid->valueint, "sl", tmp4);
	  }
	//// nastavi nazev zarizeni set device name
	if (cJSON_IsString(nazev))
	  {
	  char tmp4[11];
	  strcpy(tmp4, nazev->valuestring);
	  rs_add_buffer(rrsid->valueint, "sdn", tmp4);
	  }
	//// nastavi operacni mod
        if (cJSON_IsNumber(term_mode))
	  {
	  char tmp4[14];
	  sprintf(tmp4, "%d",  term_mode->valueint);
	  rs_add_buffer(rrsid->valueint, "sgm", tmp4);
	  }
        }
      }

    /// nastaveni thermostat okruhu
    if ((strcmp(p_tmp1, "ring/set") == 0) && (valid_json == 1))
      {
      cJSON *id = NULL;
      id = cJSON_GetObjectItemCaseSensitive(json, "ring");
      cJSON *rrsid = NULL;
      rrsid = cJSON_GetObjectItemCaseSensitive(json, "rsid");
      cJSON *name = NULL;
      name = cJSON_GetObjectItemCaseSensitive(json, "name");
      cJSON *threshold = NULL;
      threshold = cJSON_GetObjectItemCaseSensitive(json, "threshold");
      cJSON *atds = NULL;
      atds = cJSON_GetObjectItemCaseSensitive(json, "associate_tds");
      cJSON *prog = NULL;
      prog = cJSON_GetObjectItemCaseSensitive(json, "program");
      cJSON *act = NULL;
      act = cJSON_GetObjectItemCaseSensitive(json, "action");
      if (cJSON_IsNumber(id) && cJSON_IsNumber(rrsid))
        {
	/// nastaveni thersholdu pro dany ring; set ring threshold
        if (cJSON_IsNumber(threshold))
	  {
	  char tmp4[14];
	  if (threshold->valuedouble >= 16 && threshold->valuedouble <= 32)
	    {  
	    sprintf(tmp4, "%d,%.1f", id->valueint, threshold->valuedouble);
	    rs_add_buffer(rrsid->valueint, "srt", tmp4);
	    }
	  }
	/// nastaveni programu pro dany ring
	if (cJSON_IsNumber(prog))
          {
          char tmp4[14];
          sprintf(tmp4, "%d,%d", id->valueint, prog->valueint);
          rs_add_buffer(rrsid->valueint, "stp", tmp4);
          }
	/// nastaveni nazvu pro dany ring set ring name
	if (cJSON_IsString(name))
          {
	  char tmp4[14];
          sprintf(tmp4, "%d,%s", id->valueint, name->valuestring);	  
	  rs_add_buffer(rrsid->valueint, "srn", tmp4);
	  }
	if (cJSON_IsNumber(atds))
	  {
	  char tmp4[14];
	  sprintf(tmp4, "%d,%d", id->valueint, atds->valueint);
	  rs_add_buffer(rrsid->valueint, "sta", tmp4);
	  }
	if (cJSON_IsNumber(act))
          {
          char tmp4[14];
          sprintf(tmp4, "%d,%d", id->valueint, act->valueint);
          rs_add_buffer(rrsid->valueint, "sac", tmp4);
          }
        }
      }
    //// nastaveni casoveho planu
    if ((strcmp(p_tmp1, "timeplan/set") == 0) && (valid_json == 1))
      {
      cJSON *slot_id = NULL;
      slot_id = cJSON_GetObjectItemCaseSensitive(json, "id");
      cJSON *name = NULL;
      name = cJSON_GetObjectItemCaseSensitive(json, "name");
      cJSON *active = NULL;
      active = cJSON_GetObjectItemCaseSensitive(json, "active");
      cJSON *free = NULL;
      free = cJSON_GetObjectItemCaseSensitive(json, "free");
      cJSON *start_min = NULL;
      start_min = cJSON_GetObjectItemCaseSensitive(json, "start_min");
      cJSON *start_hour = NULL;
      start_hour = cJSON_GetObjectItemCaseSensitive(json, "start_hour");
      cJSON *stop_min = NULL;
      stop_min = cJSON_GetObjectItemCaseSensitive(json, "stop_min");
      cJSON *stop_hour = NULL;
      stop_hour = cJSON_GetObjectItemCaseSensitive(json, "stop_hour");
      cJSON *week_day = NULL;
      week_day = cJSON_GetObjectItemCaseSensitive(json, "week_day");
      cJSON *condition = NULL;
      condition = cJSON_GetObjectItemCaseSensitive(json, "condition");
      cJSON *threshold = NULL;
      threshold = cJSON_GetObjectItemCaseSensitive(json, "threshold");
      change = 0;
      if (cJSON_IsNumber(slot_id))
        {
	load_timeplan(slot_id->valueint, &tp);
        if (cJSON_IsString(name) && strlen(name->valuestring) < 8) {strcpy(tp.name, name->valuestring); change = 1;}
	if (cJSON_IsNumber(active)) {tp.active = active->valueint; change = 1;}
	if (cJSON_IsNumber(free)) {tp.free = free->valueint; change = 1;}
	if (cJSON_IsNumber(condition)) {tp.condition = condition->valueint; change = 1;}
	if (cJSON_IsNumber(threshold)) {tp.threshold = threshold->valuedouble; change = 1;}
        if (cJSON_IsNumber(start_min) && cJSON_IsNumber(start_hour) && cJSON_IsNumber(stop_min) && cJSON_IsNumber(stop_hour))
	  {
	  change = 1;
	  tp.start_min = start_min->valueint;
	  tp.start_hour = start_hour->valueint;
	  tp.stop_min = stop_min->valueint;
	  tp.stop_hour = stop_hour->valueint;
	  tp.week_day = week_day->valueint;
	  }
	if (change == 1) save_timeplan(slot_id->valueint, &tp);
        }
      }
    /// nastaveni programu
    if ((strcmp(p_tmp1, "programplan/set") == 0) && (valid_json == 1))
      {
      cJSON *slot_id = NULL;
      slot_id = cJSON_GetObjectItemCaseSensitive(json, "id");
      cJSON *name = NULL;
      name = cJSON_GetObjectItemCaseSensitive(json, "name");
      cJSON *active = NULL;
      active = cJSON_GetObjectItemCaseSensitive(json, "active");
      cJSON *free = NULL;
      free = cJSON_GetObjectItemCaseSensitive(json, "free");
      cJSON *tp = NULL;
      tp = cJSON_GetObjectItemCaseSensitive(json, "timeplans");
      cJSON *arr = NULL;
      change = 0;
      if (cJSON_IsNumber(slot_id))
        {
	load_programplan(slot_id->valueint, &pp);
        if (cJSON_IsString(name) && strlen(name->valuestring) < 8) {strcpy(pp.name, name->valuestring); change = 1;}
        if (cJSON_IsNumber(active)) {pp.active = active->valueint; change = 1;}
	if (cJSON_IsNumber(free)) {pp.free = free->valueint; change = 1;}
	if (cJSON_IsArray(tp))
	  {
          j = 0;
	  arr = tp->child;
	  while ((arr != NULL) && (j < 10))
	    {
	    change = 1;
	    pp.timeplan[j] = arr->valueint;
	    arr = arr->next;
	    j++;
	    }
	  }
	if (change == 1) save_programplan(slot_id->valueint, &pp);
	}
      }
    cJSON_Delete(json);
    }
  ///
  /// funkce nastaveni lokalni ds18s20
  p_tmp1[0] = 0;
  strcpy(p_tmp1, "/regulatory/");
  strcat(p_tmp1, device.nazev);
  strcat(p_tmp1, "/ds18s20/");
  len = strlen(p_tmp1);
  let = topic_len;
  if (strncmp(parse_topic, p_tmp1, len) == 0)
    {
    cJSON *json = cJSON_Parse(parse_data);
    if (json == NULL)
      {
      valid_json = 0;
      }
    else
      valid_json = 1;

    p_tmp1[0] = 0;
    j = 0;
    for (uint8_t p = len; p < let; p++)
      {
      p_tmp1[j] = topic[p];
      p_tmp1[j + 1] = 0;
      j++;
      }
    if ((strcmp(p_tmp1, "set/offset") == 0) && (valid_json == 1))
      {
      cJSON *slot_id = NULL;
      slot_id = cJSON_GetObjectItemCaseSensitive(json, "id");
      cJSON *offset = NULL;
      offset = cJSON_GetObjectItemCaseSensitive(json, "offset");
      if (cJSON_IsNumber(slot_id) && cJSON_IsNumber(offset))
        function_ds18s20_set_offset(slot_id->valueint, offset->valuedouble);
      }
    if ((strcmp(p_tmp1, "set/name") == 0) && (valid_json == 1))
      {
	cJSON *slot_id = NULL;
	slot_id = cJSON_GetObjectItemCaseSensitive(json, "id");
	cJSON *name = NULL;
	name = cJSON_GetObjectItemCaseSensitive(json, "name");
	if (cJSON_IsNumber(slot_id) && cJSON_IsString(name))
	  {
          function_ds18s20_set_name(slot_id->valueint, name->valuestring);
	  }
      }
    cJSON_Delete(json);
    }
  ///
  /// funkce pro lokalni 1wire sbernici
  p_tmp1[0] = 0;
  strcpy(p_tmp1, "/regulatory/");
  strcat(p_tmp1, device.nazev);
  strcat(p_tmp1, "/1wire/");
  len = strlen(p_tmp1);
  let = topic_len;
  if (strncmp(parse_topic, p_tmp1, len) == 0)
    {
    cJSON *json = cJSON_Parse(parse_data);
    if (json == NULL)
      {
      valid_json = 0;
      //printf("Chyba cJSON: %s\n\r", parse_data);
      }
    else
      valid_json = 1;
    p_tmp1[0] = 0;
    j = 0;
    for (uint8_t p = len; p < let; p++)
      {
      p_tmp1[j] = topic[p];
      p_tmp1[j + 1] = 0;
      j++;
      }
    //// vymazu vsechny annoncovane cidla
    if (strcmp(p_tmp1, "ds18s20/clear")) 
      {
      for (uint8_t idx = 0; idx < HW_ONEWIRE_MAXROMS; idx++)
        function_ds18s20_unannounced_rom(idx);
      printf("Mazu vsechny announcovane cidla\n\r");
      }
     //// funkce pro nastaveni annonce cidla
    if ((strcmp(p_tmp1, "ds18s20/announce") == 0) && (valid_json == 1))
      {
      cJSON *id = NULL;
      id = cJSON_GetObjectItemCaseSensitive(json, "id");
      if (cJSON_IsNumber(id))
        {
        function_ds18s20_announced_rom(id->valueint);
        }
      }
    //// smazani celeho sensoru
    if ((strcmp(p_tmp1, "ds18s20/unannounce") == 0) && (valid_json == 1))
      {
      cJSON *id = NULL;
      id = cJSON_GetObjectItemCaseSensitive(json, "id");
      if (cJSON_IsNumber(id))
        {
        function_ds18s20_unannounced_rom(id->valueint);
        }
      }
    cJSON_Delete(json);    
    }
  ///////
  p_tmp1[0] = 0;
  strcpy(p_tmp1, "/regulatory/");
  strcat(p_tmp1, device.nazev);
  strcat(p_tmp1, "/");
  len = strlen(p_tmp1);
  let = topic_len;
  if (strncmp(parse_topic, p_tmp1, len) == 0)
    {
    cJSON *json = cJSON_Parse(parse_data);
    if (json == NULL)
      {
      valid_json = 0;
      //printf("Chyba cJSON: %s\n\r", parse_data);
      }
    else
      valid_json = 1;
    ///
    p_tmp1[0] = 0;
    j = 0;
    for (uint8_t p = len; p < let; p++)
    {
     p_tmp1[j] = parse_topic[p];
     p_tmp1[j + 1] = 0;
     j++;
    }

    /// vymaze action v termostatech
    if (strcmp(p_tmp1, "actions/clear") == 0)
      {
      clear_actions(255);
      clear_actions_in_thermostat();
      printf("Clear actions\n\r");
      }

    /// nastaveni actions
    if ((strcmp(p_tmp1, "actions/set") == 0) && (valid_json == 1))
      {
      cJSON *id = NULL;
      id = cJSON_GetObjectItemCaseSensitive(json, "id");
      cJSON *free = NULL;
      free = cJSON_GetObjectItemCaseSensitive(json, "free");
      cJSON *name = NULL;
      name = cJSON_GetObjectItemCaseSensitive(json, "name");
      cJSON *force_state = NULL;
      force_state = cJSON_GetObjectItemCaseSensitive(json, "force_state");
      cJSON *outputs = NULL;
      outputs = cJSON_GetObjectItemCaseSensitive(json, "outputs");
      cJSON *arr = NULL;
      change = 0;
      if (cJSON_IsNumber(id))
        {
	 change = 1;
         load_actions(id->valueint, &at);
	 if (cJSON_IsNumber(free)){at.free = free->valueint; change = 1;};
	 if (cJSON_IsString(name)){strcpy(at.name, name->valuestring); change = 1;};
	 if (cJSON_IsNumber(force_state)){at.state = force_state->valueint; change = 1;};
	 if (cJSON_IsArray(outputs))
          {
          j = 0;
          arr = outputs->child;
          while ((arr != NULL) && (j < 5))
            {
            at.outputs[j] = arr->valueint;
            arr = arr->next;
            j++;
            }
          }
        }
      if (change == 1)
        {
        save_actions(id->valueint, &at);
        }
      }


    if (strcmp(p_tmp1, "output/set") == 0)
      {
      cJSON *c_hw_port = NULL;
      cJSON *c_id = NULL;
      cJSON *c_type = NULL;
      cJSON *c_active = NULL;
      cJSON *c_name = NULL;
      cJSON *c_mqtt_topic = NULL;
      cJSON *c_mqtt_payload_up = NULL;
      cJSON *c_mqtt_payload_down = NULL;
      cJSON *c_rs = NULL;
      cJSON *c_rsid = NULL;
      cJSON *c_rscmd = NULL;
      cJSON *c_rsargs = NULL;
      change = 0;
      c_hw_port = cJSON_GetObjectItemCaseSensitive(json, "hw");
      c_type = cJSON_GetObjectItemCaseSensitive(json, "type");
      c_id = cJSON_GetObjectItemCaseSensitive(json, "id");
      c_name = cJSON_GetObjectItemCaseSensitive(json, "name");
      c_active = cJSON_GetObjectItemCaseSensitive(json, "active");
      c_mqtt_topic = cJSON_GetObjectItemCaseSensitive(json, "mqtt_topic");
      c_mqtt_payload_up = cJSON_GetObjectItemCaseSensitive(json, "mqtt_payload_up");
      c_mqtt_payload_down = cJSON_GetObjectItemCaseSensitive(json, "mqtt_payload_down");
      c_rs = cJSON_GetObjectItemCaseSensitive(json, "rs");
      c_rsid = cJSON_GetObjectItemCaseSensitive(c_rs, "rsid");
      c_rscmd = cJSON_GetObjectItemCaseSensitive(c_rs, "cmd");
      c_rsargs = cJSON_GetObjectItemCaseSensitive(c_rs, "args");
      if  (cJSON_IsNumber(c_id))
        {
	load_output(c_id->valueint, &ot);
	if (cJSON_IsNumber(c_hw_port)){ot.hw_port = c_hw_port->valueint; change = 1;};
	if (cJSON_IsNumber(c_type)){ot.type = c_type->valueint; change = 1;};
	if (cJSON_IsString(c_name)){strcpy(ot.name, c_name->valuestring); change = 1;};
	if (cJSON_IsNumber(c_active)){ot.active = c_active->valueint; change = 1;};
	if (cJSON_IsString(c_mqtt_topic)){strcpy(ot.mqtt_topic, c_mqtt_topic->valuestring); change = 1;};
	if (cJSON_IsString(c_mqtt_payload_up)){strcpy(ot.mqtt_payload_up, c_mqtt_payload_up->valuestring); change = 1;};
	if (cJSON_IsString(c_mqtt_payload_down)){strcpy(ot.mqtt_payload_down, c_mqtt_payload_down->valuestring); change = 1;};
	if (cJSON_IsString(c_rscmd)){strcpy(ot.rs_up.cmd, c_rscmd->valuestring);  change = 1;};
	if (cJSON_IsString(c_rsargs)){strcpy(ot.rs_up.args, c_rsargs->valuestring); change = 1;};
	if (cJSON_IsNumber(c_rsid)){ot.rs_up.rsid = c_rsid->valueint; change = 1;};
        }
      if (change == 1)
        {
	save_output(c_id->valueint, &ot);
	}
      }


    ///
    if ((strcmp(p_tmp1, "at") == 0) && (valid_json == 1))
      {
      cJSON *cmd = NULL;
      cJSON *args = NULL;
      cJSON *id = NULL;
      cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
      args = cJSON_GetObjectItemCaseSensitive(json, "args");
      id = cJSON_GetObjectItemCaseSensitive(json, "rsid");
      if (cJSON_IsString(cmd) && cJSON_IsString(args) && cJSON_IsNumber(id))
        {
          rs_send_at(id->valueint, cmd->valuestring, args->valuestring);
          printf("Sending at .... \n\r");	  
        }
      }
    ///
    if (strcmp(p_tmp1, "get/network") == 0)
      {
      enable_send_network_status = 1;
      }
    ///
    if ((strcmp(p_tmp1, "set/network") == 0) && (valid_json == 1)) 
      {
      cJSON *nazev = NULL;
      nazev = cJSON_GetObjectItemCaseSensitive(json, "nazev");
      if (cJSON_IsString(nazev) && (nazev->valuestring != NULL) && strlen(nazev->valuestring) < 8)
        {
        strcpy(device.nazev, nazev->valuestring);
        printf("Novy nazev: %s\n\r", device.nazev);
        reload_network = 1;
	}
      ///
      cJSON *ip = NULL;
      ip = cJSON_GetObjectItemCaseSensitive(json, "ip");
      if (cJSON_IsString(ip) && (ip->valuestring != NULL) )
        {
        if (inet_aton(ip->valuestring, &device.myIP) != 0)
          {
          printf("Nova ip adresa: %d.%d.%d.%d\n\r", device.myIP[0], device.myIP[1], device.myIP[2], device.myIP[3]);
          reload_network = 1;
          }
        else
          printf("!! spatny format ip adresy");	
        }
      ///
      cJSON *mask = NULL;
      mask = cJSON_GetObjectItemCaseSensitive(json, "mask");
      if (cJSON_IsString(mask) && (mask->valuestring != NULL) )
        {
        if (inet_aton(mask->valuestring, &device.myMASK) != 0)
          {
          printf("Nova maska site: %d.%d.%d.%d\n\r", device.myMASK[0], device.myMASK[1], device.myMASK[2], device.myMASK[3]);
          reload_network = 1;
          }
        else
          printf("!! spatny format masky site");
        }
      ///
      cJSON *gw = NULL;
      gw = cJSON_GetObjectItemCaseSensitive(json, "gw");
      if (cJSON_IsString(gw) && (gw->valuestring != NULL) )
        {
        if (inet_aton(gw->valuestring, &device.myGW) != 0)
          {
          printf("Nova gw ip: %d.%d.%d.%d\n\r", device.myGW[0], device.myGW[1], device.myGW[2], device.myGW[3]);
          reload_network = 1;
          }
        else
          printf("!! spatny format vychozi brany");
        }
      ///
      cJSON *dns = NULL;
      dns = cJSON_GetObjectItemCaseSensitive(json, "dns");
      if (cJSON_IsString(dns) && (dns->valuestring != NULL) )
        {
        if (inet_aton(dns->valuestring, &device.myDNS) != 0)
          {
          printf("Nova dns ip: %d.%d.%d.%d\n\r", device.myDNS[0], device.myDNS[1], device.myDNS[2], device.myDNS[3]);
          reload_network = 1;
          }
        else
          printf("!! spatny format dns");
        }
      ///
      cJSON *broker = NULL;
      broker = cJSON_GetObjectItemCaseSensitive(json, "mqtt_uri");
      if (cJSON_IsString(broker) && (broker->valuestring != NULL) && (strlen(broker->valuestring) < 60))
        {
         strcpy(device.mqtt_uri, broker->valuestring);
         printf("Nova uri mqtt serveru: %s\n\r", device.mqtt_uri);
         reload_network = 1;
        }
      ///
      cJSON *essid = NULL;
      essid = cJSON_GetObjectItemCaseSensitive(json, "wifi_essid");
      if (cJSON_IsString(essid) && (essid->valuestring != NULL) && (strlen(essid->valuestring) < 20))
        {         
        printf("Novy essid %s\n\r", essid->valuestring);
        strcpy(device.wifi_essid, essid->valuestring);
        reload_network = 1;
        }
      ///
      cJSON *key = NULL;
      key = cJSON_GetObjectItemCaseSensitive(json, "wifi_pass");
      if (cJSON_IsString(key) && (key->valuestring != NULL) && (strlen(key->valuestring) < 20))
        { 
        printf("Novy essid passwd %s\n\r", key->valuestring);
        strcpy(device.wifi_pass, key->valuestring);
        reload_network = 1;
        }
      ///
      cJSON *ip_static = NULL;
      ip_static = cJSON_GetObjectItemCaseSensitive(json, "static");
      if (cJSON_IsNumber(ip_static))
        {
         device.ip_static = ip_static->valueint;
         printf("IP static %d\n\r", device.ip_static);
         reload_network = 1;
        }
      }
    cJSON_Delete(json);
    }
}
/////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
float IntToFloat(uint8_t *number)
{
  union {
    byte b[4];
    float f;
  } data;
  for (int i = 0; i < 4; i++) data.b[i] = number[i];
  return  data.f;
}

void FloatToInt(float x, uint8_t *number)
{
  union {
    byte b[4];
    float f;
  } data;
  data.f = x;
  for (int i = 0; i < 4; i++) number[i] = data.b[i];
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// nacte actions
uint8_t load_actions(uint8_t id, actions_t *at)
{
  uint8_t ret = 0;
  uint8_t low;
  if (id < max_actions)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 0, &low);
    at->last_state = low;
    for (uint8_t i = 0; i < 5; i++ )
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 1 + i, &low);
      at->outputs[i] = low;
      }
    for (uint8_t i = 0; i < 8; i++ )
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 6 + i, &low);
      at->name[i] = low;
      }
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 15, &low);
    at->free = low;
    ret = 1;
    }
  return ret;
}
/// ulozim actions
uint8_t save_actions(uint8_t id, actions_t *at)
{
  uint8_t ret = 0;
  if (id < max_actions)
    {
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 0, at->last_state);
    for (uint8_t i = 0; i < 5; i++ ) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 1 + i, at->outputs[i]);
    for (uint8_t i = 0; i < 8; i++ ) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 6 + i, at->name[i]);
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 15, at->free);
    ret = 1;
    }
  return ret;
}
/// vymazu actions 255 = vsechno
uint8_t clear_actions(uint8_t id)
{
  uint8_t ret = 0;
  actions_t at;
  /// vymazu vsechno
  if (id == 255)
    for (uint8_t ia = 0; ia < max_actions; ia++)
      {
      at.free = 0;
      sprintf(at.name, "free:%d", ia);
      for (uint8_t j = 0; j < 5; j++) at.outputs[j] = 255;
      save_actions(ia, &at);
      }
  /// vymazu pouze idcko
  if (id < max_actions)
    {
    at.free = 0;
    sprintf(at.name, "free:%d", id);
    for (uint8_t j = 0; j < 5; j++) at.outputs[j] = 255;
    ret = save_actions(id, &at);
    }
  return ret;
}
/// zjistim jestli je actions jiz vyuzita
uint8_t check_actions(uint8_t id)
{
  uint8_t ret = 0;
  uint8_t low;
  i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_actions_start + (eeprom_size_actions * id) + 15, &low);
  if (low == 1) ret = 1;
  return ret;
}
/// vymaze actions v termostatech
/// 255 = neni nic nastaveno
void clear_actions_in_thermostat(void)
{
  char tmp[8];
  for (uint8_t rsid = 0; rsid < 32; rsid++)
    {
    if (rs_device[rsid].type == ROOM_CONTROL)
      {
       for (uint8_t ring = 0; ring < 3; ring++)
         {
	 sprintf(tmp, "%d,255", ring);
         rs_add_buffer(rsid, "sac", tmp);
         }
      }
    }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// vymaze asociaci thermostat ring -> tds cidla
void clear_associate_in_thermostat(void)
{
  for (uint8_t rsid = 0; rsid < 32; rsid++)
    {
    if (rs_device[rsid].type == ROOM_CONTROL)
      {
       rs_add_buffer(rsid, "sta", "255");
      }
    }
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// nacte nastaveny vystupu
uint8_t load_output(uint8_t id, output_t *ot)
{
  uint8_t ret = 0;
  uint8_t low;
  
  if (id < max_output)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 0, &low);
    ot->hw_port = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 1, &low);
    ot->type = low;
    for (uint8_t i=0; i < 64; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 2 + i, &low);
      ot->mqtt_topic[i] = low;
      }
    for (uint8_t i=0; i < 32; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 66 + i, &low);
      ot->mqtt_payload_up[i] = low;
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 98 + i, &low);
      ot->mqtt_payload_down[i] = low;
      }
    for (uint8_t i=0; i < 8; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 130 + i, &low);
      ot->rs_up.cmd[i] = low;
      }
    for (uint8_t i=0; i < 32; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 138 + i, &low);
      ot->rs_up.args[i] = low;
      }
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 170 , &low);
    ot->rs_up.rsid = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 171 , &low);
    ot->active = low;
    for (uint8_t i=0; i < 8; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 172 + i , &low);
      ot->name[i] = low;
      }
    ret = 1;
    }
  return ret;
}
///////////////////////////////////////////////////////////////////////////
/// ulozi nastaveni vystupu
uint8_t save_output(uint8_t id, output_t *ot)
{
  uint8_t ret = 0;
  if (id < max_output)
    {
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 0, ot->hw_port);
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 1, ot->type);
    for (uint8_t i = 0; i < 64; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 2 + i, ot->mqtt_topic[i]);

    for (uint8_t i=0; i < 32; i++)
      {
      i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 66 + i, ot->mqtt_payload_up[i]);
      i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 98 + i, ot->mqtt_payload_down[i]);
      }
    for (uint8_t i = 0; i < 8; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 130 + i, ot->rs_up.cmd[i]);
    for (uint8_t i = 0; i < 32; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 138 + i, ot->rs_up.args[i]);
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 170, ot->rs_up.rsid);
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 171, ot->active);
    for (uint8_t i = 0; i < 8; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 172 + i, ot->name[i]);
    ret = 1;
    }
  return ret;
}
/// rychle zjistim jestli je pouzito
uint8_t check_output(uint8_t id)
{
  uint8_t ret = 0;
  uint8_t low;
  i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_output_start + (eeprom_size_output * id) + 171 , &low);
  if (low == 1) ret = 1;
  return ret;
}
/// vymazu nastaveny vystupu
void clear_output(uint8_t id)
{
  output_t ot;
  if (id == 255)
    {
    for (uint8_t io = 0; io < max_output; io++)
      {
      ot.hw_port = 255;
      ot.type = 255;
      ot.active = 0;
      sprintf(ot.name, "free %d", io);
      strcpy(ot.mqtt_topic, " ");
      strcpy(ot.mqtt_payload_up, "");
      strcpy(ot.mqtt_payload_down, "");
      ot.rs_up.rsid = 0;
      strcpy(ot.rs_up.cmd, " ");
      strcpy(ot.rs_up.args, " ");
      save_output(io, &ot);
      }
    }
  if (id < max_output)
    {
    ot.hw_port = 255;
    ot.type = 255;
    ot.active = 0;
    sprintf(ot.name, "free %d", id);
    strcpy(ot.mqtt_topic, "");
    strcpy(ot.mqtt_payload_up, "");
    strcpy(ot.mqtt_payload_down, "");
    ot.rs_up.rsid = 0;
    strcpy(ot.rs_up.cmd, "");
    strcpy(ot.rs_up.args, "");
    save_output(id, &ot);
    }
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// potrebuji zjistit obsazeni timeplanu, je zbytecne vsechno nacitat
uint8_t check_timeplan(uint8_t id)
{
  uint8_t ret = 0;
  uint8_t low;
  i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 6, &low);
  if (low == 1) ret = 1;
  return ret;
}
/// nacita casove plany
uint8_t load_timeplan(uint8_t id, timeplan_t *tp)
{
  uint8_t low;
  uint8_t ret = 0;
  uint8_t fl[4];
  if (id < max_timeplan )
  {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id), &low);
    tp->start_min = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 1, &low);
    tp->start_hour = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 2, &low);
    tp->stop_min = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 3, &low);
    tp->stop_hour = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 4, &low);
    tp->week_day = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 5, &low);
    tp->active = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 6, &low);
    tp->free = low;
    for (uint8_t i=0; i < 8; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 7+i, &low);
      tp->name[i] = low;
      }
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 16, &low);
    tp->condition = low;
    
    for (uint8_t i = 0; i < 4; i++) 
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 17 + i, &low);
      fl[i] = low;
      }
    
    tp->threshold = IntToFloat(fl);
    ret = 1;
  }
  return ret;
}
/// ulozi casovy plan
void save_timeplan(uint8_t id, timeplan_t *tp)
{
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id), tp->start_min);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 1, tp->start_hour);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 2, tp->stop_min);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 3, tp->stop_hour);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 4, tp->week_day);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 5, tp->active);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 6, tp->free);
  for (uint8_t i=0; i < 8; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 7+i, tp->name[i]); 
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 16, tp->condition);
  uint8_t fl[4];
  FloatToInt(tp->threshold, &fl);
  for (uint8_t i=0; i < 4; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_timeplan + (eeprom_size_timeplan * id) + 17+i, fl[i]);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// vymaze casovy plan
void clear_timeplan(uint8_t id)
{
  timeplan_t tp;
  if (id == 255)
    {
    for (uint8_t i = 0; i< max_timeplan; i++)
      {
      tp.free = 0;
      tp.active = 0;
      tp.condition = 0;
      tp.threshold = 0.0;
      sprintf(tp.name, "free %d", i);
      save_timeplan(i, &tp);
      }
    }
  else
    {
    if (id > 0 && id < max_timeplan)
      {
      tp.free = 0;
      tp.active = 0;
      tp.condition = 0;
      tp.threshold = 0;
      sprintf(tp.name, "free %d", id);
      save_timeplan(id, &tp);
      }
    }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// vymaze cele program plany
void clear_programplan(uint8_t id)
{
  programplan_t pp;
  if (id == 255)
    {
    for (uint8_t i = 0; i< max_programplan; i++)
      {
      pp.free = 0;
      pp.active = 0;
      sprintf(pp.name, "free %d", i);
      for (uint8_t j = 0; j < 10; j++) pp.timeplan[j] = 255;
      save_programplan(i, &pp);
      }
    }
  else
    {
    if (id > 0 && id < max_programplan)
      {
      pp.free = 0;
      pp.active = 0;
      sprintf(pp.name, "free %d", id);
      for (uint8_t j = 0; j < 10; j++) pp.timeplan[j] = 255;
      save_programplan(id, &pp);
      } 
    }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// rychle zjisteni obsazeni program planu
uint8_t check_programplan(uint8_t id)
{
  uint8_t ret = 0;
  uint8_t low;
  i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 1, &low);
  if (low == 1) ret = 1;
  return ret;
}
/// nacteni celeho program planu
uint8_t load_programplan(uint8_t id, programplan_t *pp)
{
  uint8_t low;
  uint8_t ret = 0;
  if (id < max_programplan)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id), &low);
    pp->active = low;
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 1, &low);
    pp->free = low;
    for (uint8_t i = 0; i < 10; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 2 + i, &low);
      pp->timeplan[i] = low;
      //printf("nacitam: %d", low);
      }
    for (uint8_t i=0; i < 8; i++)
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 12 + i, &low);
      pp->name[i] = low;
      }
    /// zde mam volny byte
    ///i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 20, &low);
    //pp->actions = low;
    ret = 1;
    }
return ret;
}

/// ulozeni celeho program planu
uint8_t save_programplan(uint8_t id, programplan_t *pp)
{
  uint8_t ret = 0;
  if (id < max_programplan)
    {
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id), pp->active);
    i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 1, pp->free);
    for (uint8_t i = 0; i < 10; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 2 +i, pp->timeplan[i]);
    for (uint8_t i = 0; i < 8; i++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 12 +i, pp->name[i]);
    /// zde mam volny byte i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_program_plan + (eeprom_size_program_plan * id) + 20, pp->actions);
    ret = 1;
    }
  return ret;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// vytvoreni noveho program planu
void new_termostat_program(void)
{
  programplan_t pp;
  for (uint8_t id = 0; id < max_programplan; id++)
    {
    load_programplan(id, &pp);
    if ((pp.free == 0) || (pp.free == 255))
      {
      pp.free = 1;
      sprintf(pp.name, "Novy %d", id);
      save_programplan(id, &pp);
      printf("Novy programplan na indexu: %d\n\r", id);
      break;
      }
    }
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// vytvoreni noveho timeplanu
void new_timeplan(void)
{
  timeplan_t tp;
  for (uint8_t id = 0; id < max_timeplan; id++)
    {
    load_timeplan(id, &tp);
    if ((tp.free == 0) || (tp.free == 255))
      {
      tp.free = 1;
      sprintf(tp.name, "Novy %d", id);
      save_timeplan(id, &tp);
      printf("Novy timeplan na indexu: %d\n\r", id);
      break;
      }
    }
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///
void mqtt_subscribe(void)
{
  char s_tmp1[64];
  strcpy(s_tmp1, "/regulatory/");
  strcat(s_tmp1, device.nazev);
  strcat(s_tmp1, "/#");
  esp_mqtt_client_subscribe(mqtt_client, s_tmp1, 0);

  strcpy(s_tmp1, "/regulatory/global/#");
  esp_mqtt_client_subscribe(mqtt_client, s_tmp1, 0);
}
///////////////////////////////////////
//



//
esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    //client = event->client;
    int msg_id = 0;
    // your_context_t *context = event->context;
    //
    //printf("mqtt event = %d\n\r", event->event_id);
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
	    mqtt_connected = 1;
            mqtt_subscribe();
            break;
        case MQTT_EVENT_DISCONNECTED:
	    mqtt_connected = 0;
            //ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            //ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            //msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            //ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            //ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            //ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            //printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            //printf("DATA=%.*s\r\n", event->data_len, event->data);
	    //procces_mqtt_json(event->topic, event->topic_len, event->data, event->data_len);
	    new_process_mqtt_message(event->topic, event->topic_len, event->data, event->data_len);
            break;
        case MQTT_EVENT_ERROR:
            //ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
	    //mqtt_connected = 0;
            break;
	case MQTT_EVENT_BEFORE_CONNECT:
	    //mqtt_connected = 0;
	    break;
        default:
            //ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }

    msg_id=msg_id;
    return ESP_OK;
}





void wifi_init(void *arg)
{
    tcpip_adapter_ip_info_t ipInfo;
    ip_addr_t dnsserver;
    nvs_flash_init();
    tcpip_adapter_init();
    if (device.ip_static == 1)
      {
      tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
      tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, device.nazev);
      IP4_ADDR(&ipInfo.ip, device.myIP[0], device.myIP[1], device.myIP[2], device.myIP[3]);
      IP4_ADDR(&ipInfo.gw, device.myGW[0], device.myGW[1], device.myGW[2], device.myGW[3]);
      IP4_ADDR(&ipInfo.netmask, device.myMASK[0], device.myMASK[1], device.myMASK[2], device.myMASK[3]);
      //inet_pton(AF_INET, "192.168.1.250", &ipInfo.ip);
      //inet_pton(AF_INET, "192.168.1.1", &ipInfo.gw);
      //inet_pton(AF_INET, "255.255.255.0", &ipInfo.netmask);
      tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
      //inet_pton(AF_INET, "192.168.1.1", &dnsserver);
      char str1[16];
      sprintf(str1, "%d.%d.%d.%d", device.myDNS[0], device.myDNS[1], device.myDNS[2], device.myDNS[3]);
      inet_pton(AF_INET, str1, &dnsserver);
      dns_setserver(0, &dnsserver);
      }
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, arg));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, (char *)device.wifi_essid);
    strcpy((char *)wifi_config.sta.password, (char *)device.wifi_pass);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    esp_wifi_start();

    //ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
//    ESP_ERROR_CHECK(esp_wifi_start());

//    esp_wifi_connect();
    //ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 1000);
    if (wifi_connected == 0) printf("wifi not connect\n\r");
}




void mqtt_init(void)
{
  esp_mqtt_client_config_t mqtt_cfg = { };
  printf("mqtt broker uri=%s\n\r", device.mqtt_uri);

  mqtt_cfg.event_handle = mqtt_event_handler; 
  mqtt_cfg.disable_auto_reconnect = false;
  mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_set_uri(mqtt_client, device.mqtt_uri);
  esp_mqtt_client_start(mqtt_client);
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void setup(void)
{

  uart_config_t uart_config = {
     .baud_rate = 38400,
     .data_bits = UART_DATA_8_BITS,
     .parity    = UART_PARITY_DISABLE,
     .stop_bits = UART_STOP_BITS_1,
     .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
     .rx_flow_ctrl_thresh = 122
  };

  uart_param_config(UART_NUM_2, &uart_config);
  uart_set_pin(UART_NUM_2, RS_TXD, RS_RXD, RS_RTS, RS_CTS);
  uart_driver_install(UART_NUM_2, RS_BUF_SIZE  , RS_BUF_SIZE, 0, NULL, 0);
  uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX);

  ds2482_address[0].i2c_addr = 0b0011000;
  ds2482_address[0].HWwirenum = 0;
  ds2482_address[0].hwwire_cekam = false;
  ds2482_address[1].i2c_addr = 0b0011011;
  ds2482_address[1].HWwirenum = 0;
  ds2482_address[1].hwwire_cekam = false;
 
  twi_init(I2C_NUM_0);

  GLCD_Setup();
  GLCD_SetFont(Font5x8, 5, 8, GLCD_Overwrite);
  GLCD_Clear();
  GLCD_GotoXY(0, 16);
  GLCD_PrintString("booting ...");
  GLCD_Render();

  //clear_timeplan(255);
  //clear_programplan(255);
  //clear_output(255);
  //clear_actions(255);
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////
////// nacteni z i2c pameti
esp_err_t i2c_eeprom_readByte(uint8_t deviceAddress, uint16_t address, uint8_t *data)
{
  esp_err_t ret;
  i2c_cmd_handle_t cmd;

  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, deviceAddress << 1 | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, (address >> 8), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, (address & 0xFF), ACK_CHECK_EN);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK)
    return ret;

  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, deviceAddress << 1 | READ_BIT, ACK_CHECK_EN);
  i2c_master_read_byte(cmd, data, NACK_VAL);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  return ret;
}

///// zapis do i2c pameti
esp_err_t i2c_eeprom_writeByte(uint8_t deviceAddress, uint16_t address, uint8_t data)
{
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, deviceAddress << 1 | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, (address >> 8), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, (address & 0xFF), ACK_CHECK_EN);
  i2c_master_write_byte(cmd, data, ACK_CHECK_EN);
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);
  return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////////////////
void load_setup_network(void)
{
  uint8_t low;
  i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 0, &low);
  device.ip_static = low;
  uint8_t m;
  /// pozice 1..5 je volne
  for (m = 0; m < 4; m++) 
    { 
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 6 + m, &low);
    device.myIP[m] = low;
    }
  for (m = 0; m < 4; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 10 + m, &low);
    device.myMASK[m] = low;
    }
  for (m = 0; m < 4; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 14 + m, &low);
    device.myGW[m] = low;
    }
  for (m = 0; m < 4; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 18 + m, &low);
    device.myDNS[m] = low;
    }
  for (m = 0; m < 9; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 22 + m, &low);
    device.nazev[m] = low;
    }
  for (m = 0; m < 60; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 35 + m, &low);
    device.mqtt_uri[m] = low;
    }
  /// poslednich 5 bytu do stovky

  for (m = 0; m < 20; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_wifi_setup + 0 + m, &low);
    device.wifi_essid[m] = low;
    }
  for (m = 0; m < 20; m++)
    {
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_wifi_setup + 20 + m, &low);
    device.wifi_pass[m] = low;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// ulozi nastaveni site
void save_setup_network(void)
{
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 0, device.ip_static);
  for (uint8_t m = 0; m < 4; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 6 + m, device.myIP[m]);
  for (uint8_t m = 0; m < 4; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 10 + m, device.myMASK[m]);
  for (uint8_t m = 0; m < 4; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 14 + m, device.myGW[m]);
  for (uint8_t m = 0; m < 4; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 18 + m, device.myDNS[m]);
  for (uint8_t m = 0; m < 9; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 22 + m, device.nazev[m]);
  for (uint8_t m = 0; m < 60; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_my_device + 35 + m, device.mqtt_uri[m]);
  for (uint8_t m = 0; m < 20; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_wifi_setup + 0 + m, device.wifi_essid[m]);
  for (uint8_t m = 0; m < 20; m++) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_wifi_setup + 20 + m, device.wifi_pass[m]);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
void load_tds18s20_from_eeprom(void)
{
  uint8_t low,high;
  for (uint8_t slot = 0; slot < HW_ONEWIRE_MAXROMS; slot++)
  {
    /// nacteni priznaku volno
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50), &low);
    tds18s20[slot].volno = low;
    /// nacteni rom adresy
    for (uint8_t a = 0; a < 8; a++ ) 
      {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 1 + a, &low);
      tds18s20[slot].rom[a] = low;
      }
    /// nacteni prizareni ke sbernici
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 9, &low);
    tds18s20[slot].assigned_ds2482 = low;
    /// nacteni nazvu cidla
    for (uint8_t a = 0; a < 20; a++ ) 
    {
      i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 10 + a, &low);
      tds18s20[slot].nazev[a] = low;
    }
    /// nacteni offestu
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 41, &high); 
    i2c_eeprom_readByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 42, &low);
    tds18s20[slot].offset = (high << 8) + low;
  }
}


void store_tds18s20_to_eeprom(uint8_t slot)
{
  /// ulozeni priznaku volno
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50), tds18s20[slot].volno);
  /// ulozeni rom adresy
  for (uint8_t a = 0; a < 8; a++ ) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 1 + a, tds18s20[slot].rom[a]);
  /// ulozeni prizareni ke sbernici
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 9, tds18s20[slot].assigned_ds2482);
  /// ulozeni nazvu cidla
  for (uint8_t a = 0; a < 20; a++ ) i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 10 + a, tds18s20[slot].nazev[a]);
  ////// todo zjistit proc tak velka dira v pameti
  /// ulozeni offsetu
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 41, (tds18s20[slot].offset >> 8) & 255);
  i2c_eeprom_writeByte(I2C_MEMORY_ADDR, eeprom_start_stored_device + (slot * 50) + 42, tds18s20[slot].offset & 255);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////
/// funkce pro porovnavani rom adress, vraci true, pokud se plne shoduje, vraci false pokud neni stejna
uint8_t compare_rom(uint8_t * rom1, uint8_t * rom2)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    if (rom1[i] != rom2[i]) return False;
  }
  return True;
}
/////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////
//// vrati slot index banky, ve ktere je ulozena dana rom
uint8_t find_by_rom_tds18s20(uint8_t *rom)
{
  uint8_t trom[8];
  uint8_t ret = 255;
  for (uint8_t i = 0; i < HW_ONEWIRE_MAXROMS; i++)
    if (tds18s20[i].volno == 1)
    {
      for (int8_t a = 0; a < 8; a++)  trom[a] = tds18s20[i].rom[a];
      if (compare_rom(rom, trom) == True)
        ret = i;
    }
  return ret;
}
//////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////
///// vrati prvni volny slot banky
uint8_t find_free_bank_tds18s20(void)
{
  for (uint8_t i = 0; i < HW_ONEWIRE_MAXROMS; i++)
  {
    if (tds18s20[i].volno == 0)
      return i;
  }
  return 255;
}
////////////////////////////////////////////////////
/// vymaze vazbu
void function_ds18s20_unannounced_rom(uint8_t slot)
{
  for (int8_t a = 0; a < 8; a++)  tds18s20[slot].rom[a] = 0;
  strcpy(tds18s20[slot].nazev, "free");
  tds18s20[slot].volno = 0;
  tds18s20[slot].assigned_ds2482 = 0;
  store_tds18s20_to_eeprom(slot);
}
/// nastavi vazbu
void function_ds18s20_announced_rom(uint8_t idx)
{
  uint8_t slot = 0;
  slot = find_free_bank_tds18s20();
  if (slot != 255)
    {
    if (find_by_rom_tds18s20(w_rom[idx].rom) == 255)
      {
      tds18s20[slot].volno = 1;
      for (int8_t a = 0; a < 8; a++)  tds18s20[slot].rom[a] = w_rom[idx].rom[a];
      tds18s20[slot].assigned_ds2482 = ds2482_address[w_rom[idx].assigned_ds2482].i2c_addr;
      store_tds18s20_to_eeprom(slot);
      }
    }
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
void function_ds18s20_set_offset(uint8_t slot, int offset)
{
  if (tds18s20[slot].volno == 1)
  {
    tds18s20[slot].offset = offset;
    //Serial.println(offset);
    store_tds18s20_to_eeprom(slot);
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////////
void function_ds18s20_set_name(uint8_t slot, const char* name)
{
  if (tds18s20[slot].volno == 1)
  {
    strcpy(tds18s20[slot].nazev, name);
    store_tds18s20_to_eeprom(slot);
  }
}

/// merime na ds18s20 
uint8_t mereni_hwwire_18s20(uint8_t maxidx)
{
  uint8_t status = 0;
  uint8_t t, e;
  for (uint8_t idx = 0; idx < maxidx; idx++)
  {
    /// pro celou sbernici pustim zacatek mereni teploty
    if (ds2482_address[idx].hwwire_cekam == false)
    {
      status = owReset(ds2482_address[idx].i2c_addr);
      status = owSkipRom(ds2482_address[idx].i2c_addr);
      status = owWriteByte(ds2482_address[idx].i2c_addr, OW_CONVERT_T);
      ds2482_address[idx].hwwire_cekam = true;
    }
    t = 0;
    status = owReadByte(ds2482_address[idx].i2c_addr, &t);
    if (t != 0) ds2482_address[idx].hwwire_cekam = false;

    if (ds2482_address[idx].hwwire_cekam == false)
      for (uint8_t w = 0; w < HW_ONEWIRE_MAXROMS; w++)
      {
        ///proverit toto se mi nezda
        if ((tds18s20[w].volno == 1) && (tds18s20[w].assigned_ds2482 == ds2482_address[idx].i2c_addr))
        {
          status = 0;
          status = status + owReset(ds2482_address[idx].i2c_addr);
          status = status + owMatchRom(ds2482_address[idx].i2c_addr, tds18s20[w].rom );
          status = status + owWriteByte(ds2482_address[idx].i2c_addr, OW_READ_SCRATCHPAD);
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e);     //0byte
          tds18s20[w].tempL = e;
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e);     //1byte
          tds18s20[w].tempH = e;
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //2byte
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //3byte
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //4byte
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //5byte
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //6byte
          tds18s20[w].CR = e; //count remain
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //7byte
          tds18s20[w].CP = e; // count per
          status = status + owReadByte(ds2482_address[idx].i2c_addr, &e); //8byte
          tds18s20[w].CRC = e; // crc soucet
          if (status == 0)
          {
            uint16_t temp = (uint16_t) tds18s20[w].tempH << 11 | (uint16_t) tds18s20[w].tempL << 3;
            tds18s20[w].temp = ((temp & 0xfff0) << 3) -  16 + (  (  (tds18s20[w].CP - tds18s20[t].CR) << 7 ) / tds18s20[w].CP ) + tds18s20[w].offset;
            tds18s20[w].online = True;
          }
          else
          {
            tds18s20[w].online = False;
          }
        }
      }
  }
  return status;
}
////////////////////////////////////////////////////////////////////////////////////
/////vyhledani zarizeni na hw 1wire sbernici////////
uint8_t one_hw_search_device(uint8_t idx)
{
  uint8_t r;
  ds2482_address[idx].HWwirenum = 0;
  ds2482init(ds2482_address[idx].i2c_addr);
  ds2482reset(ds2482_address[idx].i2c_addr);
  ds2482owReset(ds2482_address[idx].i2c_addr);
  r = owMatchFirst(ds2482_address[idx].i2c_addr, tmp_rom);
  if (r == DS2482_ERR_NO_DEVICE) {
    /*chyba zadne zarizeni na sbernici*/
  }
  if (r) {
    /*jina chyba*/
  }

  if (r == DS2482_ERR_OK)
    while (1) {
      if (ds2482_address[idx].HWwirenum > HW_ONEWIRE_MAXDEVICES - 1) break;
      for (uint8_t a = 0; a < 8; a++)  w_rom[Global_HWwirenum].rom[a] = tmp_rom[a];
      w_rom[Global_HWwirenum].assigned_ds2482 = idx;
      r = owMatchNext(ds2482_address[idx].i2c_addr, tmp_rom);
      /// celkovy pocet detekovanych roms
      ds2482_address[idx].HWwirenum++;
      Global_HWwirenum++;
      if (r == DS2482_ERR_NO_DEVICE)
      { //hledani dokonceno
        break;
      }
    }
  return r;
}

///////////////////////////////////////////////////////////////////////////////////
void check_function(void)
{
    char c_tmp1[32];
    char c_tmp2[32];
    uint8_t start_count, itmp;
    uint8_t sync_time = 0;
    for (uint8_t init = 0; init < 12; init++)
    {
      GLCD_Clear();
      GLCD_GotoXY(0, 0);
      itoa(init, c_tmp1, 10);
      strcpy(c_tmp2, "init: ");
      strcat(c_tmp2, c_tmp1);
      strcat(c_tmp2, "/12");
      GLCD_PrintString(c_tmp2);
      /// test display
      if (init == 0)
      {
        GLCD_GotoXY(0, 16);
        GLCD_PrintString("display - OK");
	GLCD_Render();
      }
      /// test eeprom
      if (init == 1)
      {
        i2c_eeprom_readByte(I2C_MEMORY_ADDR, 0, &start_count);
	start_count++;
	i2c_eeprom_writeByte(I2C_MEMORY_ADDR, 0, start_count);
	i2c_eeprom_readByte(I2C_MEMORY_ADDR, 0, &itmp);
	if (start_count == itmp)
          {
          GLCD_GotoXY(0, 16);
	  GLCD_PrintString("eeprom - OK");
	  }
        else
          {
          GLCD_GotoXY(0, 16);
	  GLCD_PrintString("eeprom - ERR");
	  }
	GLCD_Render();
      }
      /// test ds2482
      if (init == 2)
      {
        for (uint8_t f = 0; f < 2; f++)
        {
        GLCD_GotoXY(0, 9 + (9 * f));
	itoa(ds2482_address[f].i2c_addr, c_tmp1, 10);
	if (ds2482reset(ds2482_address[f].i2c_addr) == DS2482_ERR_OK)
	  {
	  strcpy(c_tmp2, "ds2482: ");
	  strcat(c_tmp2, c_tmp1);
	  strcat(c_tmp2, " = OK");
	  GLCD_PrintString(c_tmp2);
	  }
	else
	  {
	  strcpy(c_tmp2, "ds2482: ");
	  strcat(c_tmp2, c_tmp1);
	  strcat(c_tmp2, " = ERR");
	  GLCD_PrintString(c_tmp2);
	  }
        }
	GLCD_Render();
      }
      /// pocet dostupnych 1wire
      if (init == 3)
        {
        GLCD_GotoXY(0, 16);
	Global_HWwirenum = 0;
	for (uint8_t i = 0; i < 2; i++ ) one_hw_search_device(i);
	strcpy(c_tmp2, "found 1wire: ");
	itoa(Global_HWwirenum, c_tmp1, 10);
	strcat(c_tmp2, c_tmp1);
	GLCD_PrintString(c_tmp2);
	GLCD_Render();
	}
      /// nacteni informaci z eepromky
      if (init == 4)
	{
	GLCD_GotoXY(0, 16);
	GLCD_PrintString("load from eeprom");
	GLCD_Render();
	load_tds18s20_from_eeprom();
	load_setup_network();
	//strcpy(device.mqtt_uri, "mqtt://192.168.1.120");
	//save_setup_network();
	}
      /// inicializace wifi
      if (init == 5)
	{
	GLCD_GotoXY(0, 16);
	GLCD_PrintString("start wifi");
	GLCD_Render();
	static httpd_handle_t server = NULL;
	wifi_init(&server);
	}
      /// inicializace mqqt protokolu
      if (init == 6) 
	{ 
	  {
	  GLCD_GotoXY(0, 16);
	  GLCD_PrintString("init mqtt");
	  GLCD_Render();
	  mqtt_init();
	  }
	}
      /// overeni funkce RTC
      if (init == 7)
      {
	RTC_DS1307_isrunning(&itmp);
	if (!itmp)
	{
	  GLCD_GotoXY(0, 16);
	  GLCD_PrintString("RTC = OK");
	  sync_time = 1;
	}
	else
	{
	  GLCD_GotoXY(0, 12);
	  GLCD_PrintString("RTC = ERR");
	  GLCD_GotoXY(0, 20);
	  /// kdyz nebezi RTC pokusim se nacist cas z NTP
	  if (wifi_connected == 1)
	    {
	    if (sync_ntp_time_with_local_rtc() == 1)
	    {
	      GLCD_PrintString("NTP = OK");
	      sync_time = 1;
	    }
	    else
	    {
	      GLCD_PrintString("NTP = ERR");
	      sync_time = 0;
	    }
	    }
          else
	    printf("... error ntp, not wifi running\n\r");
	}
	GLCD_Render();
      }

      /// overeni stavu wifi
      if (init == 8)
      {
	GLCD_GotoXY(0, 16);
	if (wifi_connected == 1)
	{
	  strcpy(c_tmp2, "WIFI: UP ");
	  GLCD_PrintString(c_tmp2);
	}
	else
	{
	  strcpy(c_tmp2, "WIFI: DOWN ");
	  GLCD_PrintString(c_tmp2);
	}
	GLCD_Render();
      }
	 
      /// overeni stavu spojeni na mqtt, ale pokud jsem uspesne sync s internetem
      if ((init == 9) && (sync_time == 1))
      {
	GLCD_GotoXY(0, 16);
	GLCD_PrintString("Sync time at devices");
	sync_time_rs_device();
	GLCD_Render();
      }
	      
      /// ukazi na displayi
      vTaskDelay(100);
    }
    printf("%s", "ready ...\n\r");
}


////////////////////////////////////////////////////////////////////////////
void printJsonObject(cJSON *item)
{
    char *json_string = cJSON_Print(item);
    if (json_string)
    {
	printf("%s\n", json_string);
	cJSON_free(json_string);
    }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//
void show_1wire_status(char *text)
{
  char c_tmp1[8];
  strcpy(text, "1wire: ");
  itoa(Global_HWwirenum, c_tmp1, 10);
  strcat(text, c_tmp1);
}


void show_heap(char *text)
{
  char h_tmp1[8];
  strcpy(text, "mem:");
  itoa(esp_get_free_heap_size()/1024, h_tmp1, 10);
  strcat(text, h_tmp1);
  strcat(text, "kb");
}


void show_wifi_status(char *text)
{
  if (wifi_connected == 1)
     strcpy(text, "wifi - u"); 
  else
     strcpy(text, "wifi - d");
}

void show_mqtt_status(char *text)
{
  if (mqtt_connected == 1)
	  strcpy(text, "mqtt - u");
  if (mqtt_connected == 0)
	  strcpy(text, "mqtt - d");
  if (mqtt_connected == 2)
	  strcpy(text, "mqtt - ?");
}

void show_rssi(char *text)
{
  wifi_ap_record_t wifidata;
  if (wifi_connected == 1)
    {
    if (esp_wifi_sta_get_ap_info(&wifidata)==0)
       sprintf(text, "rssi:%d", wifidata.rssi);
    }
  else
    strcpy(text, "rssi:---");
}

void show_time(char *text)
{
  DateTime now;
  RTC_DS1307_now(&now);
  sprintf(text, "%.2d:%.2d:%.2d", now.hour, now.minute, now.second);
}


void show_uptime(char *text)
{
  int32_t time_since_boot = esp_timer_get_time()/1000/1000;
  sprintf(text, "rt: %ds", time_since_boot);

  if (time_since_boot > 59)
    {
    sprintf(text, "rt:%dm", time_since_boot/60);
    if (time_since_boot % 2 == 0)
      strcat(text, "*");
    }
  if (time_since_boot > (60*60)-1)
    {
    sprintf(text, "rt:%dh", time_since_boot/60/60);
    if (time_since_boot % 2 == 0)
      strcat(text, "*");
    }
  if (time_since_boot > (60*60*24)-1)
    {
    sprintf(text, "rt:%dd", time_since_boot/60/60/24);
    if (time_since_boot % 2 == 0)
      strcat(text, "*");
    }
}


//////////////////////////////////////////////////////////////////////////////////////////////////////
void show_screen(void)
{
  char str1[32];
  GLCD_Clear();
  str1[0]=0;

  GLCD_GotoXY(0, 0);
  show_time(str1);
  GLCD_PrintString(str1);

  GLCD_GotoXY(0,9);
  show_rssi(str1);
  GLCD_PrintString(str1);

  GLCD_GotoXY(64,0);
  show_mqtt_status(str1);
  GLCD_PrintString(str1);

  GLCD_GotoXY(64,9);
  show_wifi_status(str1);
  GLCD_PrintString(str1);

  GLCD_GotoXY(0,22);
  show_heap(str1);
  GLCD_PrintString(str1);

  GLCD_GotoXY(64,22);
  show_uptime(str1);
  GLCD_PrintString(str1);

  GLCD_Render();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void rs_send_buffer(void)
{
  //int32_t time_since_boot = esp_timer_get_time(); 
  uint8_t cnt = send_at[send_rsid].cnt;
  uint8_t send_idx = send_at[send_rsid].send_idx;
  //printf("rsid: %d, total cnt: %d\n\r", send_rsid, cnt);
  while (cnt > 0)
    {
    rs_send_at(send_rsid, send_at[send_rsid].data[send_idx].cmd, send_at[send_rsid].data[send_idx].args);
    //printf("time: %d, cnt:%d,  %d, %s, %s\n\r",time_since_boot ,cnt, send_rsid, send_at[send_rsid].data[send_idx].cmd, send_at[send_rsid].data[send_idx].args);
    send_idx++;
    if (send_idx > 15) send_idx = 0;

    send_at[send_rsid].send_idx = send_idx;
    cnt--;
    send_at[send_rsid].cnt = cnt;
    }

  send_rsid++;
  if (send_rsid > 32)
    send_rsid = 0;
}

/////////////////////////////////////////////////////////////////////////////////////
uint8_t rs_add_buffer(uint8_t rsid, char *cmd, char *args)
{
  uint8_t idx = send_at[rsid].idx;
  uint8_t cnt = send_at[rsid].cnt;
  uint8_t ret = 0;
  if (cnt < 16)
    {
    strcpy(send_at[rsid].data[idx].cmd, cmd);
    strcpy(send_at[rsid].data[idx].args, args);
    send_at[rsid].cnt++;
    idx++;
    if (idx > 15) idx = 0;
    send_at[rsid].idx = idx;
    ret = 1;
    }
  return ret;
}


/// funkce, ktera postupne kontaktuje rs zarizeni a ceka odpoved.
void sync_rs_device(void)
{
  rs_add_buffer(find_rsid, "sync", "NULL");
  if (rs_device[find_rsid].online == 3) rs_device[find_rsid].online = 0;
  if (rs_device[find_rsid].online == 2) rs_device[find_rsid].online = 3;
  if (rs_device[find_rsid].online == 1) rs_device[find_rsid].online = 2;
  find_rsid++;
  if (find_rsid > 32) find_rsid = 0;  
}
///////////////////////////////////////////////////
void sync_thermostat(void)
{
  uint32_t programs_available = 0;
  char tmp4[12];
  programplan_t pp;
  for (uint8_t id = 0; id < max_programplan; id++ )
    {
    if (check_programplan(id) == 1)
      { 
      programs_available = programs_available | (1 << id);
      }
    }
  sprintf(tmp4, "%d", programs_available);

  for (uint8_t rsid = 0; rsid < 32; rsid++)
    {
    if (rs_device[rsid].type == ROOM_CONTROL)
      {
       rs_add_buffer(rsid, "spc", tmp4);
      } 
    }
}
////////////////////////////////////////////////////////////////
//kdyz je rs zarizeni offline, tak vsechno nastav jako offline
//online status
// .. 0 rs zarizeni vraci chybu
// .. 1 rs vsechno je ok
// .. 2 nastaveno z duvodu necinnosti, jeste jednou pockam
// .. 3 nastaveno z duvodu necinnosti 
void sync_status(void)
{
  for (uint8_t rsid = 0; rsid < 32; rsid++)
    {
    if (rs_device[rsid].type == ROOM_CONTROL)
      {
      if (rs_device[rsid].online == 0)
        for (uint8_t idx = 0; idx < remote_tds[rsid].cnt; idx++)
		remote_tds[rsid].id[idx].online = 2;
      }
    }
}
//////////////////////////////////////////////////////////////////////////////////////////////////////
void callback_1_sec(void* arg)
{
  mereni_hwwire_18s20(2);

  //tcpip_adapter_ip_info_t ipInfo;
  //char str[256];
  /*
  // IP address.
  tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
  printf("ip: %s\n", ip4addr_ntoa(&ipInfo.ip));
  printf("netmask: %s\n", ip4addr_ntoa(&ipInfo.netmask));
  printf("gw: %s\n", ip4addr_ntoa(&ipInfo.gw));
  ip_addr_t dnssrv=dns_getserver(0);
  printf("dns=%s\n\r", inet_ntoa(dnssrv));
  */
  show_screen(); 
  //printf("free: %d\n\r", esp_get_free_heap_size());
  //int32_t time_since_boot = esp_timer_get_time()/1000/1000;
  //printf("uptime: %d\n\n", time_since_boot);
  //show_time(str);
  //printf("aktual cas: %s\n\r", str);
  //printf("----\n\r");

  sync_status();
  sync_rs_device();
 

  if (enable_send_network_status == 1)
    {
     //// odesli skutecne nastaveni, pozor rozdil mezi static a dhcp
     enable_send_network_status = 0;
     send_network_static_config();
     send_network_running_config(); 
    }

  if (reload_network == 1)
    {
    save_setup_network(); 
    }
  
}



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///udalost kazdych 30sec
void callback_30_sec(void* arg)
{
  /// hledam nove 1wire zarizeni
  Global_HWwirenum = 0;
  for (uint8_t i = 0; i < 2; i++ )
     one_hw_search_device(i);

  sync_thermostat();

  /// odesilam informace
  if (mqtt_connected == 1)
    {
    send_device_status();
    send_network_static_config();
    //send_mqtt_find_rom();
    //send_mqtt_tds();
    send_rs_device();
    send_rs_wire();
    send_rs_tds();
    send_thermostat_status();
    send_thermostat_ring_status();
    //send_programplan();
    //send_timeplan();
    //send_set_output();
    //send_actions();
    }
  else
    {
    printf("mqtt not connected\n\r");
    }
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///udalost kazdych 100ms
void callback_25_msec(void* arg)
{


}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void add_at_input_command_buffer(char *data, uint8_t len)
{
  char cmd[MAX_TEMP_BUFFER];
  char args[MAX_TEMP_BUFFER];
  char str1[MAX_TEMP_BUFFER];
  char str2[MAX_TEMP_BUFFER];
  char* token;
  uint32_t stamp;
  uint8_t rsid;
  printf("input uart: %s \n\r", data);

  stamp = esp_timer_get_time()/1000/1000;
  new_parse_at(data, str1, str2);
  rsid = atoi(str1);

  cmd[0] = 0;
  args[0] = 0;
  new_parse_at(str2, cmd, args);

  if (strcmp(cmd, "tc") == 0)
    {
    remote_tds[rsid].cnt = atoi(args);
    }

  if (strcmp(cmd, "gtdsoffset") == 0)
    {
    new_parse_at(args, str1, str2);
    remote_tds[rsid].id[atoi(str1)].offset = atoi(str2);
    }

  if (strcmp(cmd, "gt") == 0)
    {
    new_parse_at(args, str1, str2);
    remote_tds[rsid].stamp = stamp;
    if (strcmp(str2, "ERR") == 0)
      remote_tds[rsid].id[atoi(str1)].online = 0;
    else
      {
      remote_tds[rsid].id[atoi(str1)].temp = atof(str2);
      remote_tds[rsid].id[atoi(str1)].online = 1;
      }
    remote_tds[rsid].ready = 1; 
    }

  if (strcmp(cmd, "gkn") == 0)
    {
    new_parse_at(args, str1, str2);
    strcpy(remote_wire[rsid].id[atoi(str1)].name, str2);
    }
  /// pocet vzdalenych wire
  if (strcmp(cmd, "wc") == 0)
    {
    remote_wire[rsid].cnt = atoi(args);
    }
  ///
  if (strcmp(cmd, "wm") == 0)
    {
    new_parse_at(args, str1, str2);
    char* rest = str2;
    uint8_t cnt = 0;
    while ((token = strtok_r(rest, ":", &rest)) && cnt < 8) 
      {
      remote_wire[rsid].id[atoi(str1)].rom[cnt] = strtol(token, NULL, 16);
      cnt++;
      }
    remote_wire[rsid].stamp = stamp;
    remote_wire[rsid].ready = 1;
    }
  /// termostat nazev zarizeni
  if (strcmp(cmd, "gdn") == 0)
    {
    strcpy(rs_device[rsid].device_name, args);
    }
  /// termostat intenzita osvetleni
  if (strcmp(cmd, "gl") == 0)
    remote_room_thermostat[rsid].light = atoi(args);
  /// termostat globalni mode 
  if (strcmp(cmd, "ggm") == 0)
    {
    remote_room_thermostat[rsid].term_mode = atoi(args);
    remote_room_thermostat[rsid].ready = 1;
    }
  /// termostat ringy programy
  if (strcmp(cmd, "gtp") == 0)
    {
    new_parse_at(args, str1, str2);
    remote_room_thermostat[rsid].active_program[atoi(str1)] = atoi(str2);
    }
  /// termostat ringy nastaveni action
  if (strcmp(cmd, "gac") == 0)
    {
    new_parse_at(args, str1, str2);
    remote_room_thermostat[rsid].ring_action[atoi(str1)] = atoi(str2);
    }
  /// termostat nazev get ring name 
  if (strcmp(cmd, "grn") == 0)
    {
    new_parse_at(args, str1, str2);
    strcpy(remote_room_thermostat[rsid].ring_name[atoi(str1)], str2);
    }
  /// termostat nastaveni thresholdu pri rucnim programu get ring treshold
  if (strcmp(cmd, "grt") == 0)
    {
    new_parse_at(args, str1, str2);
    remote_room_thermostat[rsid].ring_threshold[atoi(str1)] = atof(str2);
    }
  /// termostat asociace cidla tds
  if (strcmp(cmd, "gta") == 0)
    {
    new_parse_at(args, str1, str2);
    remote_room_thermostat[rsid].ring_associate_tds[atoi(str1)] = atoi(str2);
    }
  /// uptime rs zarizeni
  if (strcmp(cmd, "upd") == 0)
    {
    rs_device[rsid].uptime = atoi(args);
    }

  /// zprava typu ident = identifikace zarizeni
  if (strcmp(cmd, "ident") == 0)
    {
     if (strcmp(args, "room-control OK") ==0 )
      {
      rs_device[rsid].type = ROOM_CONTROL;
      rs_device[rsid].version = 1;
      rs_device[rsid].stamp = stamp;
      rs_device[rsid].ready = 1;
      rs_device[rsid].online = 1;
      }
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void thermostat(void)
{
  for (uint8_t rsid = 0; rsid < 32; rsid++)
    {
    if ((rs_device[rsid].ready == 1) && (rs_device[rsid].online == 1) && (rs_device[rsid].type == ROOM_CONTROL))
      if (remote_room_thermostat[rsid].ready == 1)
        {
	/// vypnuto
        if (remote_room_thermostat[rsid].term_mode == 0)
	  {
          for (uint8_t ring = 0; ring < 3; ring++)
	    remote_room_thermostat[rsid].ring_action[ring]=1;	  
	  }

        }
    }
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void rs_receive_task(void* args)
//void rs_receive_task(void)
{
  uint8_t* data = (uint8_t*) malloc(RS_BUF_SIZE);
  char input[MAX_TEMP_BUFFER];
  uint16_t re_at = 0;
  uint8_t start_at = 0;
  uint8_t c;
  int len = 0;
  uint8_t xty = 0;
  while(1)
    {
    len = uart_read_bytes(UART_NUM_2, data, RS_BUF_SIZE, PACKET_READ_TICS);
    if (len > 0)
      {
      for (uint8_t p = 0; p < len; p++) 
        {
	c = data[p];
	if ( re_at > 0 && re_at < (MAX_TEMP_BUFFER - 1) )
	  {
          if (c == ';')
	    {
	    add_at_input_command_buffer(input, re_at);
	    re_at = 0;
	    start_at = 0;
	    input[0] = 0; 
	    goto endloop;
	    }
	  input[re_at - 1] = c;
	  input[re_at] = 0;
	  re_at++;
	  }        
	if (start_at == 2)
	  {
	  if (c == '+') start_at = 3;
          else start_at = 0;
          }
	if (start_at == 1)
	  {
	  if (c == 't') start_at = 2;
	  else start_at = 0;
          }
	if (start_at == 0) if (c == 'a') start_at = 1;
        if (start_at == 3) { re_at = 1; start_at = 4; }
        endloop:;	
	}	
      }
    if (xty > 32)
      {
      rs_send_buffer();
      xty = 0;
      }
    xty++;
    }
  free(data);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////
////////////////////////////////////////
void app_main()
{
  setup();
  check_function();

  const esp_timer_create_args_t periodic_timer_args_1_sec = {.callback = &callback_1_sec, .name = "1_sec" };
  const esp_timer_create_args_t periodic_timer_args_30_sec = {.callback = &callback_30_sec, .name = "30_sec" };

  xTaskCreate(rs_receive_task, "rs_task", RS_TASK_STACK_SIZE, NULL, RS_TASK_PRIO, NULL);

  
  esp_timer_handle_t periodic_timer_1_sec;
  esp_timer_create(&periodic_timer_args_1_sec, &periodic_timer_1_sec);
  esp_timer_start_periodic(periodic_timer_1_sec, 1000000);


  esp_timer_handle_t periodic_timer_30_sec;
  esp_timer_create(&periodic_timer_args_30_sec, &periodic_timer_30_sec);
  esp_timer_start_periodic(periodic_timer_30_sec, 30000000);
}



























































































    /*
    //// Print chip information /
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    for (int i = 10; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
    */


/*
    char string[200];
    strcpy(string, cJSON_Print(json));
    printf("%s", string);

    cJSON *name = NULL;
    cJSON *monitor = cJSON_CreateObject();
    name = cJSON_CreateString("Awesome 4K");
    cJSON_AddItemToObject(monitor, "name", name);

    name = cJSON_CreateString("66:66:44:33:22");
    cJSON_AddItemToObject(monitor, "mac", name);

    strcpy(string, cJSON_Print(monitor));
    cJSON_Delete(monitor);
    printf("%s", string);
*/
