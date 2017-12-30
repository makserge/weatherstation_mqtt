#include <TimeLib.h>
#include <TimeAlarms.h>
#include <Timezone.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <PubSubClient.h>

#include "ArialRoundedMTBold_14.h"
#include "ArialRoundedMTBold_36.h"

/*
 NodeMCU -> ITDB02-1.8SP
 3.3V -> VDD33
 D8   -> CS
 D5   -> SCL
 D7   -> SDA
 D3   -> RS
 D4   -> RST 
 GND  -> GND
 */

const char* WIFI_SSID = "smn-pon";
const char* WIFI_PASSWD = "***";

const byte MOTION_SENSOR_PIN = 16;
boolean lastMotionSensorState = false;

IPAddress mqttServer(192, 168, 1, 2); // mosquitto address
const char* MQTT_USER = "smhome";
const char* MQTT_PASS = "smhome";
const char* MQTT_CLIENT = "ESP8266";
const char* MQTT_TEMP_HUM_TOPIC = "weather_station/temperature_humidity";
const char* MQTT_LIGHT_LEVEL_TOPIC = "weather_station/light_level";
const char* MQTT_MOTION_SENSOR_TOPIC = "weather_station/motion_sensor";

const int SI7021_I2C_ADDRESS = 0x40;
const int BH1750_I2C_ADDRESS = 0x23;
const int BH1750_CONTINUOUS_LOW_RES_MODE = 0x13;

unsigned int lastLightLevel = 0;

TimeChangeRule EEST = {"EEST", Last, Sun, Mar, 3, 180};  //Daylight time = UTC + 3 hours
TimeChangeRule EET = {"EET", Last, Sun, Oct, 4, 120}; //Standard time = UTC + 2 hours
Timezone CE(EEST, EET);
TimeChangeRule *tcr; 

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte ntpPacketBuffer[NTP_PACKET_SIZE];

const char* NTP_SERVER = "pool.ntp.org";
const int NTP_CLIENT_PORT = 2390;
const int NTP_SERVER_PORT = 123;
const int NTP_SERVER_RETRY_DELAY = 16000;

const char* WEATHER_HOST = "api.openweathermap.org";
const int WEATHER_PORT = 80;
const char* WEATHER_CITY_ID = "703448"; //Kiev;
const char* WEATHER_KEY = "bd5e378503939ddaee76f12ad7a97608";
const int WEATHER_ATTEMPTS = 10;

const int WEATHER_BUFFER_SIZE = 1536;

const double NTP_SERVER_UPDATE_INTERVAL = 86400;
const double WEATHER_UPDATE_INTERVAL = 30 * 60;
const double TIME_UPDATE_INTERVAL = 60;
const double TIME_TICK_UPDATE_INTERVAL = 1;

const int CLOCK_FONT = 7;
const int CLOCK_X_POS = 10;
const int CLOCK_Y_POS = 80;
const int CLOCK_TICK_X_POS = 74;

const int TEMPERATURE_X_POS = 160;
const int HUMIDITY_X_POS = 130;
const int HUMIDITY_Y_POS = 39;

const int INT_TEMPERATURE_X_POS = 120;
const int INT_TEMPERATURE_Y_POS = 50;
const int INT_HUMIDITY_X_POS = 130;
const int INT_HUMIDITY_Y_POS = 55;

const unsigned int BLACK = 0x0000;
const unsigned int WHITE = 0xFFFF;

const byte MAX_WIFI_CONNECT_DELAY = 10;

WiFiUDP udp;
IPAddress ntpServerIP;

WiFiClient httpClient;

TFT_eSPI tft = TFT_eSPI();
PubSubClient mqttClient(httpClient, mqttServer);

int lastWeatherId;
int outHumidity;
float outTemp;
int weatherId;

boolean tickShown = false;

boolean isNight = false;

extern unsigned char cloud[];
extern unsigned char cloud2[];
extern unsigned char thunder[];
extern unsigned char wind[];

time_t getNTPtime() {
  time_t epoch = 0UL;
  while((epoch = getFromNTP()) == 0) {
    delay(NTP_SERVER_RETRY_DELAY);
  }
  epoch -= 2208988800UL;
  return CE.toLocal(epoch, &tcr);
}

unsigned long getFromNTP() {
  udp.begin(NTP_CLIENT_PORT);
  if(!WiFi.hostByName(NTP_SERVER, ntpServerIP)) {
    Serial.println("DNS lookup failed.");
    return 0UL;
  }
  Serial.print("sending NTP packet to ");
  Serial.print(NTP_SERVER);
  Serial.print(" ");
  Serial.println(ntpServerIP);
  memset(ntpPacketBuffer, 0, NTP_PACKET_SIZE);
  ntpPacketBuffer[0] = 0b11100011;
  ntpPacketBuffer[1] = 0;
  ntpPacketBuffer[2] = 6;
  ntpPacketBuffer[3] = 0xEC;
  ntpPacketBuffer[12]  = 49;
  ntpPacketBuffer[13]  = 0x4E;
  ntpPacketBuffer[14]  = 49;
  ntpPacketBuffer[15]  = 52;

  udp.beginPacket(ntpServerIP, NTP_SERVER_PORT);
  udp.write(ntpPacketBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
  
   // wait to see if a reply is available
  delay(1000);
  int cb = udp.parsePacket();
  if (!cb) {
    udp.flush();
    udp.stop();
    Serial.println("no packet yet");
    return 0UL;
  }
  udp.read(ntpPacketBuffer, NTP_PACKET_SIZE);
  udp.flush();
  udp.stop();
    
  unsigned long highWord = word(ntpPacketBuffer[40], ntpPacketBuffer[41]);
  unsigned long lowWord = word(ntpPacketBuffer[42], ntpPacketBuffer[43]);
  return (unsigned long) highWord << 16 | lowWord;
}

void getWeatherData() {
  getInternalTemperatureData();
  Serial.print("connecting to "); 
  Serial.println(WEATHER_HOST);
  if (httpClient.connect(WEATHER_HOST, WEATHER_PORT)) {
    httpClient.println(String("GET /data/2.5/weather?id=") + WEATHER_CITY_ID + "&units=metric&appid=" + WEATHER_KEY + "&lang=en\r\n" +
                "Host: " + WEATHER_HOST + "\r\nUser-Agent: ArduinoWiFi/1.1\r\n" +
                "Connection: close\r\n\r\n");
  }
  else {
    Serial.println("connection failed");
    return;
  }
  int repeatCounter = 0;
  while (!httpClient.available() && repeatCounter < WEATHER_ATTEMPTS) {
    delay(500);
    repeatCounter++;
  }
  String weatherData = "";
  while (httpClient.connected() && httpClient.available()) {
    char c = httpClient.read(); 
    if (c == '[' || c == ']') c = ' ';
    weatherData += c;
  }
  httpClient.stop();
  StaticJsonBuffer<WEATHER_BUFFER_SIZE> jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(weatherData);
  if (!root.success()) {
    Serial.println("parseObject() failed");
    return;
  }
  outTemp = root["main"]["temp"];
  outHumidity = root["main"]["humidity"];
  weatherId = root["weather"]["id"];
   
  if (isnan(outTemp) || outTemp < -50 || outTemp > 50 || isnan(weatherId)) {
    return;
  }

  tft.fillRect(86, 0, 160, 50, BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFreeFont(&ArialRoundedMTBold_36);
  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(tft.textWidth("-88`"));
  String temp = "";
  if ((int)outTemp < 0) {
    temp = "-";
  }
  else if ((int)outTemp > 0) {
    temp = "+";
  }
  temp += String((int)outTemp) + "`";
  tft.drawString(temp, TEMPERATURE_X_POS, 0);

  tft.setCursor(HUMIDITY_X_POS, HUMIDITY_Y_POS);
  tft.setTextFont(0);
  tft.print(String(outHumidity) + "%");

  if (weatherId != lastWeatherId) {
    lastWeatherId = weatherId;
    printWeatherIcon(weatherId);
  }
}

void printWeatherIcon(int id) {
  tft.fillRect(0, 0, 88, CLOCK_Y_POS - 1, BLACK);
  switch(id) {
    case 800: 
      drawClearWeather();
      break;
    case 801:
    case 802:
      drawFewClouds();
      break;
    case 803: 
    case 804:
      drawCloud(); 
      break;
    case 200: 
    case 201:
    case 202:
    case 210:
    case 211:
    case 212:
    case 221:
    case 230:
    case 231:
    case 232: 
      drawThunderstorm();
      break;
    case 300: 
    case 301:
    case 302:
    case 310: 
    case 311: 
    case 312:
    case 313:
    case 314: 
    case 321: 
    case 511:
      drawLightRain();
      break;
    case 500: 
    case 501: 
    case 502:
    case 503: 
    case 504: 
      drawLightRainWithSunOrMoon();
      break;
    case 520:
    case 521:
      drawModerateRain(); 
      break;
    case 522:
    case 531: 
      drawHeavyRain(); 
      break;
    case 600: 
    case 611:
    case 612:
    case 615:
    case 616:
    case 620:
      drawLightSnowfall();
      break;
    case 601: 
    case 621: 
      drawModerateSnowfall(); 
      break;
    case 602: 
    case 622:
      drawHeavySnowfall(); 
      break;
    case 701: 
    case 711: 
    case 721: 
    case 731: 
    case 741: 
    case 751: 
    case 761: 
    case 762: 
    case 771: 
    case 781: 
      drawFog(); 
      break;
    default:
      break; 
 }
}

void drawClearWeather() {
  if (isNight) {
    drawTheMoon();
  }
  else {
    drawTheSun();
  }
}

void drawFewClouds() {
  if (isNight) {
    drawCloudWithMoon();
  }
  else {
    drawCloudWithSun();
  }
}

void drawTheSun() {
  tft.fillCircle(40, 33, 26, WHITE);
}

void drawTheMoon() {
  tft.fillCircle(40, 33, 26, WHITE);
  tft.fillCircle(51, 26, 26, BLACK);
}

void drawCloud() {
  tft.drawBitmap(0, 16, cloud, 80, 43, BLACK);
  tft.drawBitmap(0, 20, cloud, 80, 43, WHITE);
}

void drawCloudWithSun() {
  tft.fillCircle(55, 25, 20, WHITE);
  tft.drawBitmap(0, 16, cloud, 80, 43, BLACK);
  tft.drawBitmap(0, 20, cloud, 80, 43, WHITE);
}

void drawCloudWithMoon() {
  tft.fillCircle(72, 25, 18, WHITE);
  tft.fillCircle(83, 18, 18, BLACK);
  tft.drawBitmap(0, 16, cloud, 80, 43, BLACK);
  tft.drawBitmap(0, 20, cloud, 80, 43, WHITE);
}

void drawThunderstorm() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.drawBitmap(31, 48, thunder, 16, 19, WHITE);
  tft.fillRoundRect(23, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(30, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(49, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(57, 52, 3, 15, 1, WHITE);
}

void drawLightRain() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.fillRoundRect(25, 55, 3, 13, 1, WHITE);
  tft.fillRoundRect(40, 55, 3, 13, 1, WHITE);
  tft.fillRoundRect(55, 55, 3, 13, 1, WHITE);
}

void drawModerateRain() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.fillRoundRect(25, 55, 3, 15, 1, WHITE);
  tft.fillRoundRect(32, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(40, 55, 3, 15, 1, WHITE);
  tft.fillRoundRect(47, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(55, 55, 3, 15, 1, WHITE);
}

void drawHeavyRain() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.fillRoundRect(18, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(25, 55, 3, 15, 1, WHITE);
  tft.fillRoundRect(32, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(40, 5, 3, 15, 1, WHITE);
  tft.fillRoundRect(47, 52, 3, 15, 1, WHITE);
  tft.fillRoundRect(55, 55, 3, 15, 1, WHITE);
  tft.fillRoundRect(62, 52, 3, 15, 1, WHITE);
}

void drawCloudWithMoonAndRain() {
  tft.fillCircle(72, 25, 18, WHITE);
  tft.fillCircle(83, 18, 18, BLACK);
  tft.drawBitmap(0, 16, cloud, 80, 43, BLACK);
  tft.drawBitmap(0, 20, cloud, 80, 43, WHITE);
  tft.fillRoundRect(25, 65, 3, 11, 1, WHITE);
  tft.fillRoundRect(40, 65, 3, 11, 1, WHITE);
  tft.fillRoundRect(55, 65, 3, 11, 1, WHITE);
}

void drawCloudWithSunAndRain() {
  tft.fillCircle(55, 25, 20, WHITE);
  tft.drawBitmap(0, 16, cloud, 80, 43, BLACK);
  tft.drawBitmap(0, 20, cloud, 80, 43, WHITE);
  tft.fillRoundRect(25, 65, 3, 13, 1, WHITE);
  tft.fillRoundRect(40, 65, 3, 13, 1, WHITE);
  tft.fillRoundRect(55, 65, 3, 13, 1, WHITE);
}

void drawLightRainWithSunOrMoon() {
  if (isNight) {
    drawCloudWithMoonAndRain();
  }
  else {
    drawCloudWithSunAndRain();
  }
}

void drawLightSnowfall() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.fillCircle(25, 55, 3, WHITE);
  tft.fillCircle(40, 58, 3, WHITE);
  tft.fillCircle(57, 55, 3, WHITE);
}

void drawModerateSnowfall() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.fillCircle(25, 55, 3, WHITE);
  tft.fillCircle(25, 65, 3, WHITE);
  tft.fillCircle(40, 58, 3, WHITE);
  tft.fillCircle(40, 58, 3, WHITE);
  tft.fillCircle(57, 55, 3, WHITE);
  tft.fillCircle(57, 65, 3, WHITE);
}

void drawHeavySnowfall() {
  tft.drawBitmap(0, 5, cloud, 80, 43, WHITE);
  tft.fillCircle(15, 55, 3, WHITE);
  tft.fillCircle(27, 55, 3, WHITE);
  tft.fillCircle(27, 65, 3, WHITE);
  tft.fillCircle(40, 58, 3, WHITE);
  tft.fillCircle(40, 68, 3, WHITE);
  tft.fillCircle(55, 55, 3, WHITE);
  tft.fillCircle(55, 65, 3, WHITE);
  tft.fillCircle(67, 55, 3, WHITE);     
}

void drawFog() {
  tft.fillRoundRect(20, 10, 40, 4, 1, WHITE);
  tft.fillRoundRect(15, 20, 50, 4, 1, WHITE);
  tft.fillRoundRect(10, 30, 60, 4, 1, WHITE);
  tft.fillRoundRect(15, 40, 50, 4, 1, WHITE);
  tft.fillRoundRect(20, 50, 40, 4, 1, WHITE);
}

void showMessage(String message) {
  tft.setFreeFont(&ArialRoundedMTBold_14);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(message, 0, 0);
}

void waitForWifiConnection() {
  showMessage("Connecting to wifi");
  
  Serial.print("Connecting");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && (retries < MAX_WIFI_CONNECT_DELAY)) {
    retries++;
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    restart();
  }
  tft.fillScreen(TFT_BLACK);
}

void getInternalTemperatureData() {
  unsigned int data[2];
 
  Wire.beginTransmission(SI7021_I2C_ADDRESS);
  Wire.write(0xF5);
  Wire.endTransmission();
  delay(500);
 
  Wire.requestFrom(SI7021_I2C_ADDRESS, 2);
  if (Wire.available() == 2) {
    data[0] = Wire.read();
    data[1] = Wire.read();
  }
  float intHumidity  = ((data[0] * 256.0) + data[1]);
  intHumidity = ((125 * intHumidity) / 65536.0) - 6;
 
  Wire.beginTransmission(SI7021_I2C_ADDRESS);
  Wire.write(0xF3);
  Wire.endTransmission();
  delay(500);
 
  Wire.requestFrom(SI7021_I2C_ADDRESS, 2);
  if (Wire.available() == 2) {
    data[0] = Wire.read();
    data[1] = Wire.read();
  }
  float temp  = ((data[0] * 256.0) + data[1]);
  float intTemp = ((175.72 * temp) / 65536.0) - 46.85;
 
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFreeFont(&ArialRoundedMTBold_14);
  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(tft.textWidth("-88`"));
  tft.drawString("+" + String((int)intTemp) + "`", INT_TEMPERATURE_X_POS, INT_TEMPERATURE_Y_POS);

  tft.setCursor(INT_HUMIDITY_X_POS, INT_HUMIDITY_Y_POS);
  tft.setTextFont(0); 
  tft.print(String((int)intHumidity) + "%");

  String payload = "{\"temp\": ";
  payload += intTemp;
  payload += ", \"humidity\": ";
  payload += intHumidity;
  payload += "}";
  publishMqtt(MQTT_TEMP_HUM_TOPIC, payload);
}

void publishMqtt(String pubTopic, String payload) {
  if (mqttClient.connected()){
    Serial.print("Sending payload: ");
    Serial.print(payload);
    Serial.println();
    Serial.print("to topic: ");
    Serial.println(pubTopic);
    if (mqttClient.publish(pubTopic, (char*) payload.c_str())) {
      Serial.println("MQTT Publish OK");
    }
    else {
      restart();
    }
  }
  else {
    Serial.println("Connecting to MQTT broker ");
    if (mqttClient.connect(MQTT::Connect(MQTT_CLIENT).set_auth(MQTT_USER, MQTT_PASS))) {
      Serial.println("Connected to MQTT broker");
      publishMqtt(pubTopic, payload);
    }
    else {
      restart();
    }
  }      
}

void showTimeTick() {
  tickShown = !tickShown;
  tft.setTextColor(tickShown ? TFT_WHITE : TFT_BLACK, TFT_BLACK);
  tft.drawChar(':', CLOCK_TICK_X_POS, CLOCK_Y_POS, CLOCK_FONT);
}

void showTime() {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int hours = hour();
  int minutes = minute();
  int xPosition = CLOCK_X_POS;
  if (hours < 10) {
    xPosition += tft.drawChar('0', xPosition, CLOCK_Y_POS, CLOCK_FONT);
  }
  xPosition += tft.drawNumber(hours, xPosition, CLOCK_Y_POS, CLOCK_FONT);
  xPosition += tft.drawChar(':', xPosition, CLOCK_Y_POS, CLOCK_FONT);
  if (minutes < 10) {
    xPosition += tft.drawChar('0', xPosition, CLOCK_Y_POS, CLOCK_FONT);
  }
  tft.drawNumber(minutes, xPosition, CLOCK_Y_POS, CLOCK_FONT);

  isNight = ((hours > 21) || (hours < 7));
}

void restart() {
    Serial.println("Will reset and try again...");
    //abort();
}

void bh1750Init() {
  Wire.beginTransmission(BH1750_I2C_ADDRESS);
  Wire.write(BH1750_CONTINUOUS_LOW_RES_MODE);
  Wire.endTransmission();
}

void readLightLevel() {
  unsigned int level;

  Wire.beginTransmission(BH1750_I2C_ADDRESS);
  Wire.requestFrom(BH1750_I2C_ADDRESS, 2);
  level = Wire.read();
  level <<= 8;
  level |= Wire.read();
  Wire.endTransmission();

  level = level / 1.2; // convert to lux

  if (level != lastLightLevel) {
    lastLightLevel = level;

    Serial.print("Light: ");
    Serial.print(level);
    Serial.println(" lx");

    String payload = "{\"level\": ";
    payload += level;
    payload += "}";
    publishMqtt(MQTT_LIGHT_LEVEL_TOPIC, payload);
  }  
} 

void motionSensorInit() {
  pinMode(MOTION_SENSOR_PIN, INPUT);
}

void readMotionSensor() {
  boolean sensorState = digitalRead(MOTION_SENSOR_PIN);
  if (sensorState != lastMotionSensorState) {
    lastMotionSensorState = sensorState;
    
    Serial.print("Motion sensor state: ");
    Serial.println(sensorState);
    
    String payload = "{\"state\": ";
    payload += sensorState;
    payload += "}";
    publishMqtt(MQTT_MOTION_SENSOR_TOPIC, payload);
  }
}

void setup() {
  Wire.begin();
  bh1750Init();
  motionSensorInit();
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWD);
  
  waitForWifiConnection();

  delay(1000);
  setSyncProvider(getNTPtime);
  setSyncInterval(NTP_SERVER_UPDATE_INTERVAL);

  showTime();
  getWeatherData();

  Alarm.timerRepeat(WEATHER_UPDATE_INTERVAL, getWeatherData);
  Alarm.timerRepeat(TIME_UPDATE_INTERVAL, showTime);
  Alarm.timerRepeat(TIME_TICK_UPDATE_INTERVAL, showTimeTick);
}

void loop() {
  readLightLevel();
  readMotionSensor();
  Alarm.delay(100);
}
