/* UDP reporting Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "udp.h"

#include <sys/socket.h>
#include <esp_spi_flash.h>
#include <esp_phy_init.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event_loop.h>
#include <rom/uart.h>		// for uart_tx_wait_idle()
#include <driver/timer.h>	// for timer_get_counter_time_sec()
#include <soc/efuse_reg.h>	// needed by getChip*()
#include <driver/adc.h>

// best to provide the following in CFLAGS
#ifndef AP_SSID
#error undefined AP_SSID
#endif
#ifndef AP_PASS
#error undefined AP_SSID
#endif

#define SVR_IP		"192.168.2.7"
#define SVR_PORT	21883

#ifndef MY_IP
#error undefined MY_IP
#endif

#define MY_NM		"255.255.255.0"
#define MY_GW		SVR_IP

#define SLEEP_S		5	// seconds
#define GRACE_MS	50	// multiple of 10ms

#define USE_DHCPC	0	// 0=no, use static IP
#define DISCONNECT	0	// 0=no disconnect before deep sleep

#define RUN_AS_TASK	1

#define READ_BME280	1	// enable when it starts working...
#define READ_DS18B20	1
#define READ_ADC	1
#define PRINT_MSG	0	// print message when logging is off

#define I2C_SCL		22	// i2c
#define I2C_SDA		21	// i2c
#define OUT_PIN		19	// OUT toggled to mark program steps
#define OW_PIN		18	// ow
#define ERROR_PIN	17	// indicate a failure (debug)
#define TOGGLE_PIN	16	// IN  pull low to disable toggle()
#define DBG_PIN		15	// IN  pull low to silence Log()

#if READ_BME280
#include "bme280.h"
#endif

#if READ_DS18B20
#include "ds18b20.h"
  #define ROM_ID	NULL
//#define ROM_ID	(uint8_t *)"\x28\xdc\x01\x78\x06\x00\x00\x0f"
#endif

#if READ_ADC
#include "adc.h"
#endif

RTC_DATA_ATTR static int runCount = 0;
RTC_DATA_ATTR static uint64_t sleep_start_us = 0;
RTC_DATA_ATTR static int failSoft = 0;
RTC_DATA_ATTR static int failHard = 0;
RTC_DATA_ATTR static int failRead = 0;
RTC_DATA_ATTR static int failReadHard = 0;
RTC_DATA_ATTR static int lastGrace = 0;
RTC_DATA_ATTR static uint64_t timeLast = 0;
RTC_DATA_ATTR static uint64_t timeTotal = 0;

static uint64_t app_start_us = 0;
static uint64_t time_wifi_us = 0;

static struct timeval app_start;
static int wakeup_cause;
static int reset_reason;
static int rssi = 0;
static int do_toggle = 0;
static int done = 0;
static int retry_count = 0;

int do_log = 0;

uint64_t rtc_time_get(void);
uint64_t _get_rtc_time_us(void);
uint64_t _get_time_since_boot(void);


static uint8_t getChipRevision (void)
{
	return (REG_READ (EFUSE_BLK0_RDATA3_REG)>>EFUSE_RD_CHIP_VER_RESERVE_S)&&EFUSE_RD_CHIP_VER_RESERVE_V;
}

static uint64_t getChipMAC (void)
{
	uint64_t mac = 
 (REG_READ (EFUSE_BLK0_RDATA2_REG)>>EFUSE_RD_WIFI_MAC_CRC_HIGH_S)&&EFUSE_RD_WIFI_MAC_CRC_HIGH_V;
	mac = (mac << 32) |
((REG_READ (EFUSE_BLK0_RDATA1_REG)>>EFUSE_RD_WIFI_MAC_CRC_LOW_S) &&EFUSE_RD_WIFI_MAC_CRC_LOW_V);
	return mac;
}

static void toggle_setup (void)
{
	gpio_pad_select_gpio(OUT_PIN);
	gpio_set_direction(OUT_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(OUT_PIN, 1);	// start HIGH
}

// pin is LOW  during sleep
// pin is HIGH at start
// toggle is 100us: 50us LOW, 50us HIGH (not on final time)
void toggle(int ntimes) {
	if (do_toggle) while (ntimes-- > 0) {
		gpio_set_level(OUT_PIN, 0);
		delay_us (50);
		gpio_set_level(OUT_PIN, 1);
		if (ntimes)
			delay_us (50);
	}
}

static void toggle_error (void)
{
	gpio_pad_select_gpio(ERROR_PIN);
	gpio_set_direction(ERROR_PIN, GPIO_MODE_OUTPUT);
	gpio_set_level(ERROR_PIN, 1);	// start HIGH
	delay_us (50);
	gpio_set_level(ERROR_PIN, 0);
}

void get_time (struct timeval *now)
{
	gettimeofday (now, NULL);
	if (now->tv_usec < app_start.tv_usec) {
		now->tv_usec += 1000000;
		--now->tv_sec;
	}

	now->tv_sec  -= app_start.tv_sec;
	now->tv_usec -= app_start.tv_usec;
}

#if READ_DS18B20
static esp_err_t ds18b20_temp (float *temp)
{
	uint8_t id[8];

	*temp = 0.0;

	DbgR (ds18b20_init (OW_PIN, ROM_ID));

	if (reset_reason != DEEPSLEEP_RESET) {	// cold start
		DbgR (ds18b20_read_id (id));
		Log("ROM id: %02x %02x %02x %02x %02x %02x %02x %02x",
			id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);

		DbgR (ds18b20_convert (1));
	}
	DbgR (ds18b20_read_temp (temp));
	if (reset_reason == DEEPSLEEP_RESET)	// deep sleep wakeup
		DbgR (ds18b20_convert (0));

	DbgR (ds18b20_depower ());

	return ESP_OK;
}
#endif // READ_DS18B20

#if 000
From include/rom/rtc.h:

typedef enum {
    NO_SLEEP        = 0,
    EXT_EVENT0_TRIG = BIT0,
    EXT_EVENT1_TRIG = BIT1,
    GPIO_TRIG       = BIT2,
    TIMER_EXPIRE    = BIT3,
    SDIO_TRIG       = BIT4,
    MAC_TRIG        = BIT5,
    UART0_TRIG      = BIT6,
    UART1_TRIG      = BIT7,
    TOUCH_TRIG      = BIT8,
    SAR_TRIG        = BIT9,
    BT_TRIG         = BIT10
} WAKEUP_REASON;	// from rtc_get_wakeup_cause()

typedef enum {
    NO_MEAN                =  0,
    POWERON_RESET          =  1,    /**<1, Vbat power on reset*/
    SW_RESET               =  3,    /**<3, Software reset digital core*/
    OWDT_RESET             =  4,    /**<4, Legacy watch dog reset digital core*/
    DEEPSLEEP_RESET        =  5,    /**<3, Deep Sleep reset digital core*/
    SDIO_RESET             =  6,    /**<6, Reset by SLC module, reset digital core*/
    TG0WDT_SYS_RESET       =  7,    /**<7, Timer Group0 Watch dog reset digital core*/
    TG1WDT_SYS_RESET       =  8,    /**<8, Timer Group1 Watch dog reset digital core*/
    RTCWDT_SYS_RESET       =  9,    /**<9, RTC Watch dog Reset digital core*/
    INTRUSION_RESET        = 10,    /**<10, Instrusion tested to reset CPU*/
    TGWDT_CPU_RESET        = 11,    /**<11, Time Group reset CPU*/
    SW_CPU_RESET           = 12,    /**<12, Software reset CPU*/
    RTCWDT_CPU_RESET       = 13,    /**<13, RTC Watch dog Reset CPU*/
    EXT_CPU_RESET          = 14,    /**<14, for APP CPU, reseted by PRO CPU*/
    RTCWDT_BROWN_OUT_RESET = 15,    /**<15, Reset when the vdd voltage is not stable*/
    RTCWDT_RTC_RESET       = 16     /**<16, RTC Watch dog reset digital core and rtc module*/
} RESET_REASON;		// from rtc_get_reset_reason()
#endif

static void format_msg (char *buf, int buflen)
{
	esp_err_t ret;
	struct timeval now;
	uint64_t us;
	char *msg = buf;
	int mlen = buflen;
	int len;
	float temp, temps[2];
	int ntemps;
	int i;
	float bat, vdd;
	char weather[40];

	if (sleep_start_us > 0)
		us = app_start_us - sleep_start_us;
	else
		us = 0;

Log("sleep_start=%lld app_start=%lld sleep_time=%lld",
	sleep_start_us, app_start_us, app_start_us-sleep_start_us);

	ntemps = 0;
#if READ_DS18B20
	Dbg (ds18b20_temp (&temp));
	if (ret != ESP_OK || temp >= BAD_TEMP) {
		Dbg (ds18b20_temp (&temp));	// one retry
		if (ret != ESP_OK || temp >= BAD_TEMP) {
			toggle_error();		// tell DSO
			++failReadHard;
			temp = BAD_TEMP;
		} else
			++failRead;
	}
	temps[ntemps++] = temp;
#endif // READ_DS18B20

#if READ_BME280
    {
	float qfe, h, qnh;
	int fail;

	Dbg (bme280_read (622, &temp, &qfe, &h, &qnh));
	if (ret != ESP_OK || temp >= BAD_TEMP) {
		toggle_error();		// tell DSO
		++failRead;
	}

	fail = 0;
	if (       BAD_TEMP <= temp) fail |= 0x01;
	if (BME280_BAD_QFE  == qfe)  fail |= 0x02;
	if (BME280_BAD_HUMI == h)    fail |= 0x04;

	snprintf (weather, sizeof(weather),
		" w=T%.2f,P%.3f,H%.3f,f%x",
		temp, qnh, h, fail);
    }
	temps[ntemps++] = temp;
#else
	weather[0] = '\0';
#endif // READ_BME280

#if READ_ADC
	Dbg (read_adc (&bat, &vdd));
#else
	bat = 5.0;
	vdd = 3.3;
#endif // READ_ADC

	get_time (&now);

	len = snprintf (msg, mlen,
		"show esp-32a %d",
		runCount);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

	len = snprintf (msg, mlen,
		" times=D%lld,s%u.%06u,w%.3f,t%u.%06u",
		us,
		(uint)app_start.tv_sec, (uint)app_start.tv_usec,
		time_wifi_us / 1000000.,
		(uint)now.tv_sec, (uint)now.tv_usec);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

	len = snprintf (msg, mlen,
		" prev=L%.3f,T%d",
		timeLast / 1000000.,
		(uint32_t)(timeTotal / 1000000));
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

    {
	uint64_t ticks = rtc_time_get();		// raw RTC time
	uint64_t RTC   = _get_rtc_time_us();		// adjusted rtc_time_get()
	uint64_t FRC   = _get_time_since_boot();	// FRC
	len = snprintf (msg, mlen,
		" clocks=R%llu,F%llu,t%llu,g%d",
		RTC, FRC, ticks, lastGrace);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}
    }

#if 000
    {
	esp_err_t ret;
	double tmr_00_sec = 0;
	Dbg (timer_get_counter_time_sec (0, 0, &tmr_00_sec));
	double tmr_01_sec = 0;
	Dbg (timer_get_counter_time_sec (0, 1, &tmr_01_sec)) ;
	double tmr_10_sec = 0;
	Dbg (timer_get_counter_time_sec (1, 0, &tmr_10_sec));
	double tmr_11_sec = 0;
	Dbg (timer_get_counter_time_sec (1, 1, &tmr_11_sec));
	len = snprintf (msg, mlen,
		" timers=%.6f,%.6f,%.6f,%.6f",
		tmr_00_sec, tmr_01_sec, tmr_10_sec, tmr_11_sec);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}
    }
#endif

	len = snprintf (msg, mlen,
		" stats=fs%d,fh%d,fr%d,fR%d,c%03x,r%d",
		failSoft, failHard, failRead, failReadHard, wakeup_cause, reset_reason);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

	len = snprintf (msg, mlen,
		"%s",
		weather);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

	len = snprintf (msg, mlen,
		" adc=%.3f vdd=%.3f",
		bat, vdd);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

	len = snprintf (msg, mlen,
		" radio=s%d",
		-rssi);
	if (len > 0) {
		msg += len;
		mlen -= len;
	}

	for (i = 0; i < ntemps; ++i) {
		len = snprintf (msg, mlen,
			"%c%.2f",
			(i ? ',' : ' '), temps[i]);
		if (len > 0) {
			msg += len;
			mlen -= len;
		}
	}

#if PRINT_MSG
	if (!do_log) {
		printf ("%s\n", msg);
		fflush(stdout);
		uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
	}
#endif

	++runCount;
}

static void send_msg (void)
{
	int mysocket;
	struct sockaddr_in remote_addr;
	char msg[500];

	mysocket = socket(AF_INET, SOCK_DGRAM, 0);
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(SVR_PORT);
	remote_addr.sin_addr.s_addr = inet_addr(SVR_IP);

	format_msg (msg, sizeof(msg));

Log ("Sending '%s'", msg);
	toggle(2);
	sendto(mysocket, msg, strlen(msg), 0,
		(struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

static void do_grace (void)
{
#if defined(GRACE_MS) && GRACE_MS > 0
	toggle(3);
Log ("delay %dms", GRACE_MS);
	uint64_t grace_us = _get_time_since_boot();
	delay_ms (GRACE_MS);	// time to drain wifi queue
	lastGrace = (int)(_get_time_since_boot() - grace_us);
#endif
}

static void finish (void)
{
	if (retry_count > 1)
		++failHard;
	else if (retry_count > 0)
		++failSoft;

#if !DISCONNECT
	do_grace ();
#endif

Log ("esp_deep_sleep %ds", SLEEP_S);
#if !LOG_FLUSH
	if (do_log) fflush(stdout);
#endif

	uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);

	toggle(4);
	sleep_start_us = _get_time_since_boot();
	timeLast = sleep_start_us - app_start_us;
	timeTotal += timeLast;
	esp_deep_sleep(SLEEP_S*1000000);
#if RUN_AS_TASK
	vTaskDelete(NULL);
#endif
}

static void set_ip (void)
{
#if !USE_DHCPC
Log ("tcpip_adapter_dhcpc_stop");
	tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);

Log ("tcpip_adapter_set_ip_info");
	tcpip_adapter_ip_info_t ip_info_new;
	memset (&ip_info_new, 0, sizeof(ip_info_new));
	ip4addr_aton(MY_IP, &ip_info_new.ip);
	ip4addr_aton(MY_NM, &ip_info_new.netmask);
	ip4addr_aton(MY_GW, &ip_info_new.gw);
	tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info_new);
#endif	/* if !USE_DHCPC */
}

static esp_err_t event_handler (void *ctx, system_event_t *event)
{
	switch(event->event_id) {
	case SYSTEM_EVENT_STA_START:
		toggle(1);
Log ("SYSTEM_EVENT_STA_START");
		break;
	case SYSTEM_EVENT_STA_CONNECTED:
		toggle(1);
		time_wifi_us = _get_time_since_boot() - time_wifi_us;
{
		wifi_ap_record_t wifidata;
		esp_wifi_sta_get_ap_info(&wifidata);
		rssi = wifidata.rssi;
Log("SYSTEM_EVENT_STA_CONNECTED rssi=%d", rssi);
}
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		toggle(1);
{
char ip[16], nm[16], gw[16];
Log ("SYSTEM_EVENT_STA_GOT_IP ip=%s nm=%s gw=%s",
	ip4addr_ntoa_r(&event->event_info.got_ip.ip_info.ip, ip, sizeof(ip)),
	ip4addr_ntoa_r(&event->event_info.got_ip.ip_info.netmask, nm, sizeof(nm)),
	ip4addr_ntoa_r(&event->event_info.got_ip.ip_info.gw, gw, sizeof(gw)));
}
		send_msg();

#if DISCONNECT
		done = 1;
		do_grace ();

Log ("esp_wifi_disconnect");
		esp_wifi_disconnect();
#else
		finish ();
#endif
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
Log ("SYSTEM_EVENT_STA_DISCONNECTED");

		if (done || ++retry_count > 1)
			finish ();
		else {
Log ("esp_wifi_connect");
			esp_wifi_connect();	// try once again
		}
		break;
	default:
Log ("ignoring SYSTEM_EVENT %d", event->event_id);
		break;
	}
	return ESP_OK;
}

static void udp(void *param)
{
#if 000		// no effect
Log ("esp_phy_load_cal_and_init");
	esp_phy_load_cal_and_init();	// no effect
#endif
	
Log ("tcpip_adapter_init");
	tcpip_adapter_init();

	set_ip();

Log ("esp_event_loop_init");
	esp_event_loop_init(event_handler, NULL);

Log ("esp_wifi_init");
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&cfg);

	if (reset_reason != DEEPSLEEP_RESET) {	// this should be saved in flash
Log ("esp_wifi_set_mode");
		esp_wifi_set_mode(WIFI_MODE_STA);

		wifi_config_t wifi_config = {
			.sta = {
				.ssid     = AP_SSID,
				.password = AP_PASS,
				.bssid_set = 0
			},
		};
Log ("esp_wifi_set_config");
		esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
	}

Log ("esp_wifi_start");
	time_wifi_us = _get_time_since_boot();
	esp_wifi_start();

Log ("esp_wifi_connect");
	esp_wifi_connect();
}

void app_main ()
{
	app_start_us = _get_time_since_boot();
	gettimeofday (&app_start, NULL);

	gpio_pad_select_gpio(TOGGLE_PIN);
	gpio_set_direction(TOGGLE_PIN, GPIO_MODE_INPUT);
	gpio_pullup_en(TOGGLE_PIN);
	do_toggle = gpio_get_level(TOGGLE_PIN);
	if (do_toggle)
		toggle_setup();

	gpio_pad_select_gpio(DBG_PIN);
	gpio_set_direction(DBG_PIN, GPIO_MODE_INPUT);
	gpio_pullup_en(DBG_PIN);
	do_log = gpio_get_level(DBG_PIN);
// turn off internal messages below ERROR level
	if (!do_log) esp_log_level_set("*", ESP_LOG_ERROR);

//Log ("app_main start portTICK_PERIOD_MS=%d sizeof(int)=%d sizeof(long)=%d",
//	portTICK_PERIOD_MS, sizeof(int), sizeof(long));

	Log ("Chip revision %d MAC %012llx", getChipRevision(), getChipMAC());

	wakeup_cause = rtc_get_wakeup_cause();
	reset_reason = rtc_get_reset_reason(0);

// do we need a RTC_DATA_ATTR magic?

#if READ_BME280
	esp_err_t ret;
	Dbg (bme280_init(I2C_SDA, I2C_SCL, reset_reason != DEEPSLEEP_RESET));
#endif // READ_BME280

#if RUN_AS_TASK
	udp(NULL);
#else
	xTaskCreate(udp, "udp", 40960, NULL, 20, NULL);
	while (1) vTaskDelay(5000/portTICK_PERIOD_MS);	// in 5s we will sleep anyway
#endif
}

#if 000	///////////////////////////////// notes ///////////////////////////////

adc
===
	4.3 Ultra-Low-Noise Analog Pre-Amplifier
	4.6 Temperature Sensor

clock sources
=============
	32KHz crystal clock (external)
		8KHz by /4
	150KHz RC (internal)
	8MHz (internal)
		->31.25KHz by /256
	4 GP timers - 64 bits
	Main WDT
	RTC WDT

Drawing in the technical doco 3.2.1 shows the RTC can be clocked from
slow:
	RTC_CLK		150.00KHz
	XTL32K_CLK	 32.00KHz
	8M_D256_CLK	 31.25KHz
fast:
	RTC8M_CLK	8MHz
	XTL_CLK/DIV

xTaskCreatePinnedToCore(&bleAdvtTask, "bleAdvtTask", 2048, NULL, 5, NULL, 0);
vTaskDelete(NULL);

xTaskCreate(eth_task, "eth_task", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL);
xTaskCreate(&blink_task, "blink_task", 512, NULL, 5, NULL);

TaskHandle_t tx_rx_task;
xTaskCreate(&send_data, "send_data", 4096, NULL, 4, &tx_rx_task);
vTaskDelete(tx_rx_task);

#endif

