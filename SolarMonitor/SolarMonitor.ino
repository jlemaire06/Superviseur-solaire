// SolarMonitor.ino

/***********************************************************************************
  Solar monitoring of a Fronius Primo GEN24 Plus inverter using an Heltec Wifi Kit 32 v2.1 card.
  
  Functionnalities :
  - initialization of the current date and time in the setup, these being supplied by an NTP server accessed via Wifi ;
  - periodic calculation of the powers (W) of PV Production and Consumption, by accessing the Fronius Solar API via WiFi ;
  - periodic estimation of the battery voltage (V), measured on pin 37 of the card ;
  - periodic display of all these values on the board OLED screen ;
  - calculation of the average powers over 5 min intervals and recording in a circular buffer ;
  - periodic emptying of the buffer on the SD card, with creation of a delimited text file (.csv) for each day ;
  - led blinking to indicate that the SD card must not be removed or inserted in its reader ;
  - processing of pressures on the 4 push buttons to carry out the following actions :
    o card reset ;
    o change to the OLED display, to view the daily energy (kWh) produced and consumed, estimated from the SD card files ;
    o going backward one day ;
    o going forward one day. 
 
  References :
  - https://riton-duino.blogspot.com/2021/06/esp32-heltec-wifi-kit-32.html
  - https://github.com/adafruit/Adafruit_SSD1306
  - https://github.com/adafruit/Adafruit-GFX-Library
  - https://randomnerdtutorials.com/esp32-useful-wi-fi-functions-arduino/
  - https://github.com/arduino-libraries/Arduino_JSON 
  - https://randomnerdtutorials.com/esp32-http-get-post-arduino/
  - https://github.com/pfeerick/elapsedMillis
  - https://randomnerdtutorials.com/esp32-date-time-ntp-client-server-arduino/
  - https://sourceware.org/newlib/libc.html#Timefns 
  - https://github.com/espressif/arduino-esp32/blob/master/cores/esp32/esp32-hal-time.c#L47 
  - https://RandomNerdTutorials.com/esp32-microsd-card-arduino/
  - https://github.com/espressif/arduino-esp32/tree/master/libraries/SD
  - https://github.com/jlemaire06/Esp32-async-multi-button-library 
  - https://github.com/pfeerick/elapsedMillis

***********************************************************************************/

/***********************************************************************************
  Libraries and structures
***********************************************************************************/

// OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSerif9pt7b.h>

// Wifi
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>

// Time
#include <time.h>                 

// Ring buffer
#include <RingBuffer.hpp>

struct Record
{
  uint8_t day;        // Day 
  uint8_t month;      // Month
  uint16_t year;      // Year
  uint8_t hour;       // Hour
  uint8_t minute;     // Minute
  uint16_t prod;      // Production(W) 
  uint16_t cons;      // Consumption(W)
};

// SD
#include <SD.h>

// Timers
#include <elapsedMillis.h>

// Display modes
enum DisplayMode {POWER, ENERGY};

// Buttons
#include <MButton.h>


/***********************************************************************************
  Constants
***********************************************************************************/

// Screen
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define SCREEN_ADDRESS 0x3C

// SD card
#define CS_SD 5

// Wifi access point
const char *SSID = "Livebox-486c";
const char *PWD = "FAAD971F372AA5CCE13D25969E";

// Web service host address of the Fronius inverter
const char *HOST = "192.168.1.13";

// Battery
#define VBAT_PIN 37     // GPIO connected to VBAT in Heltec Wifi kit 32 V2.1
#define KV 1.7E-3       // Coefficient to convert in Volts the analog reads

// NTP Server
const char* NTP_SERVER = "pool.ntp.org";  // Server address
const long  GMT_OFFSET = 3600;            // GMT+1 zone
const int   DAY_LIGHT_OFFSET = 3600;      // Summer time

// Save frequency in 1 hour 
#define SAVE_FREQUENCY 12                 // => 5mn period

// Update periods (ms)
#define RECORD_PERIOD 2000                // 2s
#define SAVE_PERIOD 300000                // 5mn
#define DISPLAY_PERIOD 1000               // 1s
#define LED_PERIOD 200                    // 200ms

// Led blinking max number (need to be odd to switch off the led at last)
#define MAX_LED_BLINK 11

// Button pins
#define RESET_PIN 12
#define MODE_PIN 27
#define BACKWARD_PIN 14
#define FORWARD_PIN 13


/***********************************************************************************
  Global variables
***********************************************************************************/

// SSD1306 display connected to I2C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, RST_OLED);

// Timers 
elapsedMillis tmrRecord;
elapsedMillis tmrSave;
elapsedMillis tmrDisplay;
elapsedMillis tmrLed;

// Parameters
struct tm timeInfo;        // Current time
double batt;               // battery voltage(V)
double prod;               // Production power(W)
double cons;               // Consumption power(W)

// Records
Record rec;                // Current record
double sumProd, sumCons;   // Sums of production and consumption values
int nbData;                // Number of data in that sums
RingBuffer<Record, SAVE_FREQUENCY> rb; // => Can store 1 hour of records

// Led state
uint8_t ledState;          // LOW/HIGH
uint8_t ledBlink;          // Led blinking count

// Display
DisplayMode dispMode;

// Buttons
MButton mButton;

// Energy 
uint8_t dayE;              // Day 
uint8_t monthE;            // Month
uint16_t yearE;            // Year 
double prodE;              // Production(kWh) 
double consE;              // Consumption(kWh)
uint16_t iDay;             // Index of the day before


/***********************************************************************************
  Functions
***********************************************************************************/

void connectToWiFi(const char * ssid, const char * pwd)
{
  // Wifi connection
  WiFi.begin(ssid, pwd);
  while (WiFi.status() != WL_CONNECTED) delay(500);
}

bool EnergyEval(const uint8_t _dayE, const uint8_t _monthE, const uint16_t _yearE, double *_prodE, double *_consE)
{
  // Energy evaluation using a file DD-MM-YYYY.csv on the SD card
  *_prodE = 0;
  *_consE = 0;
  if (!SD.begin(CS_SD)) return false;
  char buf[20];
  sprintf(buf, "/%02d-%02d-%d.csv\0", _dayE, _monthE, _yearE);
  File file = SD.open(buf, FILE_READ);
  if (!file) 
  {
    SD.end();
    return false;
  }
  file.readStringUntil('\n');
  file.readStringUntil('\n');
  file.readStringUntil('\n');
  String str;
  uint8_t i1, i2;
  while (file.available())
  {
    str = file.readStringUntil('\n');
    i1 = str.indexOf(';');
    i2 = str.indexOf(';', i1+1);
    *_prodE += (str.substring(i1+1, i2)).toInt();
    *_consE += (str.substring(i2+1)).toInt();
  }
  file.close();
  SD.end();
  *_prodE /= SAVE_FREQUENCY*1000;
  *_consE /= SAVE_FREQUENCY*1000;
  return true;
}

void changeDate(uint8_t *_day, uint8_t *_month, uint16_t *_year, long int _sec)
{
  // Change a date according an offset in seconds
  struct tm t;
  t.tm_sec = 0;
  t.tm_min = 0;
  t.tm_hour = 0;
  t.tm_mday = *_day;   
  t.tm_mon = *_month - 1;
  t.tm_year = *_year - 1900; 
  t.tm_wday = 0;
  t.tm_yday = 0;
  t.tm_isdst = 0;
  time_t tt = mktime(&t) + _sec;
  localtime_r(&tt, &t);
  *_day = t.tm_mday;
  *_month = t.tm_mon + 1;
  *_year = t.tm_year + 1900; 
}

void setup() 
{
  // Init OLED
  Wire1.begin(SDA_OLED, SCL_OLED);
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) for(;;);
  display.setTextColor(WHITE);

  // Connect to the Wifi access point 
  connectToWiFi(SSID, PWD);

  // Init time
  configTime(GMT_OFFSET, DAY_LIGHT_OFFSET, NTP_SERVER);
  while (!getLocalTime(&timeInfo)) delay(100);
  
  // Init record
  rec.day = timeInfo.tm_mday;
  rec.month = timeInfo.tm_mon + 1;    // tm_mon  = month, between 0 (January) and 11 (December).
  rec.year = timeInfo.tm_year + 1900; // tm_year = year since 1900
  rec.hour = timeInfo.tm_hour;
  rec.minute = timeInfo.tm_min - timeInfo.tm_min%(60/SAVE_FREQUENCY);
  sumProd = 0;
  sumCons = 0;
  nbData = 0;
  
  // Init timer to immediatly update
  tmrRecord = RECORD_PERIOD;

  // Initialize digital pin LED_BUILTIN as an output
  pinMode(LED_BUILTIN, OUTPUT);

  // Initialize the Led
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); 
  ledState = LOW;
  ledBlink = MAX_LED_BLINK;
  
  // Initialize display mode
  dispMode = POWER;

  // Buttons
  mButton.begin(4, RESET_PIN, MODE_PIN, BACKWARD_PIN, FORWARD_PIN);
}

void loop() 
{
  if (tmrRecord > RECORD_PERIOD)
  {
    // Reset timer
    tmrRecord = 0;
    
    // Get battery voltage
    batt = KV*analogRead(VBAT_PIN);
    
    // Record flag
    bool okRecord = true;
    
    // Get time
    okRecord = getLocalTime(&timeInfo);
    
    // Get Production and Consumption
    if(WiFi.status() == WL_CONNECTED)
    {
      // Local variables
      HTTPClient http;

      // Request Production
      http.begin("http://" + String(HOST) + "/solar_api/v1/GetInverterRealtimeData.cgi?Scope=System");
      if(http.GET() == HTTP_CODE_OK) prod = double(JSON.parse(http.getString())["Body"]["Data"]["PAC"]["Values"]["1"]);
      else okRecord = false;
      http.end();    

      // Request Consumption
      http.begin("http://" + String(HOST) + "/solar_api/v1/GetMeterRealtimeData.cgi");
      if(http.GET() == HTTP_CODE_OK) cons = prod + double(JSON.parse(http.getString())["Body"]["Data"]["0"]["PowerReal_P_Phase_1"]);
      else okRecord = false;
      http.end();    
    }
    else connectToWiFi(SSID, PWD); // WiFi reconnect
    
    // Recording
    if (okRecord)
    {
      sumProd += prod;
      sumCons += cons;
      nbData++;
      if ((timeInfo.tm_min%(60/SAVE_FREQUENCY) == 0) && (timeInfo.tm_min != rec.minute))
      {
        // Means
        if (nbData > 0)
        {
          rec.prod = round(sumProd/nbData);
          rec.cons = round(sumCons/nbData);
        }
        else
        {
          rec.prod = 0;
          rec.cons = 0;
        }
         
        // Save record in the Ring Buffer
        rb.Push(&rec);
        
        // Reinit record
        rec.day = timeInfo.tm_mday;
        rec.month = timeInfo.tm_mon + 1;
        rec.year = timeInfo.tm_year + 1900;
        rec.hour = timeInfo.tm_hour;
        rec.minute = timeInfo.tm_min;
        sumProd = 0;
        sumCons = 0;
        nbData = 0;
      }
    }  
  }
  
  if (tmrSave > SAVE_PERIOD)
  {
    // Reset timer
    tmrSave = 0;

    // Turn the LED on
    digitalWrite(LED_BUILTIN, HIGH); 
    ledState = HIGH;
    ledBlink = 0;
    tmrLed = 0;

    // Save to SD card
    if (SD.begin(CS_SD) && (rb.Size() > 0))  // SD card mounted and any record(s) to save
    {
      // Save to SD   
      bool okSave = true;
      while (okSave && (rb.Size() > 0))
      {
        // Update okSave
        okSave = false;
 
        // Read the record to save in the ring buffer
        Record r;
        rb.Get(&r, 0);

        // File name
        char buf[41];
        sprintf(buf, "/%02d-%02d-%d.csv", r.day, r.month, r.year);

        // Saving
        File file;
        if (SD.exists(buf))
        {
          file = SD.open(buf, FILE_APPEND);
          if (file)  // The file exists
          {
            sprintf(buf, "%02d:%02d;%d;%d\n", r.hour,r.minute, r.prod, r.cons);
            file.print(buf);
            file.close();
            okSave = true;
          }
        }
        else // The file doesn't exist, then create it
        {
          file = SD.open(buf, FILE_WRITE);
          if (file)
          {
            sprintf(buf, "%02d/%02d/%d;;\n;;\n", r.day, r.month, r.year);
            file.print(buf);
            sprintf(buf, "%s;%s;%s\n", "Heure", "Production(W)", "Consommation(W)");
            file.print(buf);
            sprintf(buf, "%02d:%02d;%d;%d\n", r.hour,r.minute, r.prod, r.cons);
            file.print(buf);
            file.close();
            okSave = true;
          }
        }

        // Pop the ring buffer if okSave
        if (okSave) rb.Pop(&r);
      }

      // To enable ejection
      SD.end();
    }
  } 

  if (tmrDisplay > DISPLAY_PERIOD)
  {
    // Reset timer
    tmrDisplay = 0;
    
    // OLED display
    display.clearDisplay();
    display.setFont();
    display.drawRect(93,0, 32, 13, 1);
    display.setCursor(97, 3);
    display.print(batt, 1);
    display.println('V');
    char buf[15];
    switch (dispMode)
    {
      case POWER:
        strftime(buf,15, "%d/%m/%y %H:%M", &timeInfo);
        display.setCursor(0, 3);
        display.print(buf);
        display.setFont(&FreeSerif9pt7b);
        display.setCursor(0, 35); 
        display.print("Prod ");
        display.setCursor(40, 35);     
        display.print(prod, 0); // No decimal
        display.println(" W");
        display.setCursor(0, 55); 
        display.print("Cons "); 
        display.setCursor(40, 55); 
        display.print(cons, 0); // No decimal
        display.println(" W");
        break;
      case ENERGY:
        sprintf(buf, "%02d/%02d/%d", dayE, monthE, yearE);
        display.setCursor(0, 3);
        display.print(buf);
        display.setFont(&FreeSerif9pt7b);
        display.setCursor(0, 35); 
        display.print("Prod ");
        display.setCursor(40, 35);     
        display.print(prodE, 1); // 1 decimal
        display.println(" kWh");
        display.setCursor(0, 55); 
        display.print("Cons "); 
        display.setCursor(40, 55); 
        display.print(consE, 1); // 1 decimal
        display.println(" kWh");
        break;
    }
    display.display();
  }

  if ((ledBlink < MAX_LED_BLINK) && (tmrLed > LED_PERIOD))
  {
    // Reset timer and update the led blinking count
    tmrLed = 0;
    ledBlink++; 
    
    // Switch on/off the LED
    if (ledState == HIGH)
    {
      digitalWrite(LED_BUILTIN, LOW);  
      ledState = LOW;
    }
    else
    {
      digitalWrite(LED_BUILTIN, HIGH);  
      ledState = HIGH;
    }
  }

  if (mButton.toProcess())
  {
    switch (mButton.getNum())
    {
      case RESET_PIN: // Reset
        ESP.restart();
        break;
      case MODE_PIN: // Display mode
        switch (dispMode)
        {
          case POWER:
            dispMode = ENERGY;
            dayE = timeInfo.tm_mday;
            monthE = timeInfo.tm_mon + 1;
            yearE = timeInfo.tm_year + 1900;
            EnergyEval(dayE, monthE, yearE, &prodE, &consE);
            iDay = 0;
            break;
          case ENERGY:
            dispMode = POWER;
            break;            
        }
        break;      
      case BACKWARD_PIN: // Going back a day
        if (dispMode == ENERGY) 
        {
          changeDate(&dayE, &monthE, &yearE, -86400);
          EnergyEval(dayE, monthE, yearE, &prodE, &consE);
          iDay++;
        }
        break;  
      case FORWARD_PIN: // Advance one day
        if ((dispMode == ENERGY) && (iDay > 0)) 
        {
          changeDate(&dayE,&monthE, &yearE, +86400);
          EnergyEval(dayE, monthE, yearE, &prodE, &consE);
          iDay--; 
        }
        break;        
    }
    mButton.processed(); 
  }  
}
