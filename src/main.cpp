#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Button.h>
#include <Fonts/FreeMono12pt7b.h>
#include <Fonts/FreeMono18pt7b.h>
#include <Fonts/FreeMono24pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_display_selection_new_style.h>
#include <OpenWeather.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <TimeLib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <time.h>

const char* daysOfTheWeek[8] = {"???", "Sun", "Mon", "Tue",
                                "Wed", "Thu", "Fri", "Sat"};

// Useful for debugging to go right to the weather screen
#define FAST_BOOT
const uint32_t SERIAL_SPEED = 115200;

const uint8_t USER_BTN_PIN = 27;
const gpio_num_t USER_BTN_RTC_PIN = GPIO_NUM_27;

const char* CONFIG_AP_NAME = "WeatherStationConfig";

const uint32_t FAIL_RETRY_TIME = 2; // minutes
const uint32_t UPDATE_TIME = 10;    // minutes

const char* NTP_SERVER = "pool.ntp.org";

uint32_t TZ_OFFSET = 0;               // seconds
uint16_t DAYLIGHT_SAVINGS_OFFSET = 0; // seconds

const size_t API_KEY_SIZE = 32 + 1;
char apiKey[API_KEY_SIZE] = "";

const size_t LAT_LONG_SIZE = 10 + 1;
char latitude[LAT_LONG_SIZE] = "";
char longitude[LAT_LONG_SIZE] = "";

const size_t UNITS_SIZE = 8 + 1;
char units[UNITS_SIZE] = "imperial";

const size_t LANG_SIZE = 2 + 1;
char lang[LANG_SIZE] = "en";

Button userBtn(USER_BTN_PIN);

OW_Weather ow;
OW_current current;
OW_hourly hourly;
OW_daily daily;

const uint8_t OW_GEOREV_STR_SIZE = 64 + 1;
// clang-format off
struct OW_GeocodingReverse {
  char name[OW_GEOREV_STR_SIZE] = {0};
  char state[OW_GEOREV_STR_SIZE] = {0};
  char country[OW_GEOREV_STR_SIZE] = {0};
};
// clang-format on
RTC_DATA_ATTR OW_GeocodingReverse georev;

// clang-format off
struct OW_extra {
  float temp_min;
  float temp_max;
  int32_t timezone;
};
// clang-format on
RTC_DATA_ATTR OW_extra extra;

struct tm timeInfo;

RTC_DATA_ATTR bool lastUpdateSuccess = false;

void printWakeupReason() {
  esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();

  switch (reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by external signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Wakeup caused by touch");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("Wakeup caused by ULP program");
      break;
    default:
      Serial.printf("Wakeup was not caused by deep sleep: %d\n", reason);
      break;
  }
}

void loadParams() {
  Serial.println("Loading weather configuration into memory");
  Preferences prefs;
  prefs.begin("weatherConfig", false);
  prefs.getString("apiKey", apiKey, API_KEY_SIZE);
  prefs.getString("latitude", latitude, LAT_LONG_SIZE);
  prefs.getString("longitude", longitude, LAT_LONG_SIZE);
  prefs.getString("units", units, UNITS_SIZE);
  prefs.getString("lang", lang, LANG_SIZE);
  prefs.end();
}

void saveParams() {
  Serial.println("Saving weather configuration into memory");
  Preferences prefs;
  prefs.begin("weatherConfig", false);
  prefs.putString("apiKey", apiKey);
  prefs.putString("latitude", latitude);
  prefs.putString("longitude", longitude);
  prefs.putString("units", units);
  prefs.putString("lang", lang);
  prefs.end();
}

bool connectToWiFi(bool useScreen) {
  bool startedConfigAP = false;
  bool displayedAboutStartedConfigAP = false;
  bool configAPTimedOut = false;
  bool displayedAboutConfigAPTimedOut = false;
  bool shouldSaveParams = false;

  loadParams();

  WiFiManagerParameter customAPIKey("apiKey", "OpenWeather API key", apiKey,
                                    API_KEY_SIZE);
  WiFiManagerParameter customLatitude(
      "latitude", "Latitude (need >=4 decimals)", latitude, LAT_LONG_SIZE);
  WiFiManagerParameter customLongitude(
      "longitude", "Longitude (need >=4 decimals)", longitude, LAT_LONG_SIZE);
  WiFiManagerParameter customUnits("units", "Units (imperial or metric)", units,
                                   UNITS_SIZE);
  WiFiManagerParameter customLang("lang", "2-letter language", lang, LANG_SIZE);
  WiFiManager wm;
  wm.addParameter(&customAPIKey);
  wm.addParameter(&customLatitude);
  wm.addParameter(&customLongitude);
  wm.addParameter(&customUnits);
  wm.addParameter(&customLang);

  WiFi.mode(WIFI_STA);
  wm.setAPCallback(
      [&startedConfigAP](WiFiManager* wm) { startedConfigAP = true; });
  wm.setConfigPortalTimeoutCallback(
      [&configAPTimedOut]() { configAPTimedOut = true; });
  wm.setSaveConfigCallback([&shouldSaveParams]() { shouldSaveParams = true; });
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(60);
  Serial.println("Attempting connection to WiFi");

  display.setFont(&FreeMono9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.fillScreen(GxEPD_WHITE);

  display.setCursor(0, 10);

  if (userBtn.read() == Button::PRESSED) {
    wm.resetSettings();
    Serial.println("WiFi configuration deleted.");
    display.println("WiFi configuration deleted.");
  }

  display.println("Connecting to WiFi...");
  if (useScreen) {
    display.display();
  }

  if (!wm.autoConnect(CONFIG_AP_NAME)) {
    while (true) {
      if (wm.process()) {
        break;
      }
      if (startedConfigAP && !displayedAboutStartedConfigAP) {
        Serial.println("Failed to connect to WiFi, starting configuration AP.");
        Serial.print("Join the WiFi network \"");
        Serial.print(CONFIG_AP_NAME);
        Serial.println("\" and open http://192.168.4.1 to open the WiFi "
                       "credential configuration page.");
        display.println(
            "Failed to connect to WiFi, starting configuration AP.");
        display.print("Join the WiFi network \"");
        display.print(CONFIG_AP_NAME);
        display.println("\" and open http://192.168.4.1 to open the WiFi "
                        "credential configuration page.");
        if (useScreen) {
          display.display();
        }
        displayedAboutStartedConfigAP = true;
      }
      if (configAPTimedOut && !displayedAboutConfigAPTimedOut) {
        Serial.println("Configuration AP timed out, exiting.");
        display.println("Configuration AP timed out, exiting.");
        displayedAboutConfigAPTimedOut = true;
        goto wifiConnectFailed;
      }
    }
  }

  Serial.println("Successfully connected to saved WiFi network!");
  Serial.print("Connected to: ");
  Serial.println(WiFi.SSID());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
  Serial.print("Local IPv4 address: ");
  Serial.println(WiFi.localIP());

  Serial.println("Getting parameters");
  strncpy(apiKey, customAPIKey.getValue(), API_KEY_SIZE);
  strncpy(latitude, customLatitude.getValue(), LAT_LONG_SIZE);
  strncpy(longitude, customLongitude.getValue(), LAT_LONG_SIZE);
  strncpy(units, customUnits.getValue(), UNITS_SIZE);
  strncpy(lang, customLang.getValue(), LANG_SIZE);
  Serial.println("Parameters: ");
  Serial.print("apiKey=");
  Serial.println(apiKey);
  Serial.print("latitude=");
  Serial.println(latitude);
  Serial.print("longitude=");
  Serial.println(longitude);
  Serial.print("units=");
  Serial.println(units);
  Serial.print("lang=");
  Serial.println(lang);
  if (shouldSaveParams) {
    Serial.println("User configured parameters, saving");
    saveParams();
  }

  display.println("Successfully connected to WiFi!");
  if (useScreen) {
    display.display();
    delay(5000);
  }
  return true;

wifiConnectFailed:
  Serial.println("Failed to connect to WiFi!");
  display.println("Failed to connect to WiFi!");
  if (useScreen) {
    display.display();
    delay(5000);
  }
  return false;
}

bool disconnectFromWiFi() {
  Serial.println("Disconnecting and turning WiFi off!");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_wifi_stop();

  return true;
}

bool updateTime() {
  Serial.println("Configuring time");
  configTime(TZ_OFFSET, DAYLIGHT_SAVINGS_OFFSET, NTP_SERVER);
  if (!getLocalTime(&timeInfo)) {
    Serial.println("Failed to obtain time");
    return false;
  }
  Serial.print("Time is ");
  Serial.println(&timeInfo, "%A, %B %d %Y %H:%M:%S");
  return true;
}

bool updateGeocodingReverse() {
  Serial.println("Calling reverse geocoding API");
  Serial.print("Latitude: ");
  Serial.println(latitude);
  Serial.print("Longitude: ");
  Serial.println(longitude);
  WiFiClientSecure client;
  client.setInsecure();
  const char* host = "api.openweathermap.org";
  const uint16_t port = 443;
  if (!client.connect(host, port)) {
    Serial.println("Connection failed");
    return false;
  }
  client.print("GET ");
  client.print("/geo/1.0/reverse?lat=");
  client.print(latitude);
  client.print("&lon=");
  client.print(longitude);
  client.print("&limit=1&lang=");
  client.print(lang);
  client.print("en&appid=");
  client.print(apiKey);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  client.print("\r\nConnection: close\r\n\r\n");
  if (client.println() == 0) {
    Serial.println("Failed to send request");
    return false;
  }
  Serial.println("Sent request, pulling out header");
  if (!client.find("\r\n\r\n")) {
    Serial.println("Could not find end of headers");
    return false;
  }
  Serial.println("Found end of header");
  Serial.println("Parsing JSON");
  StaticJsonDocument<1536> doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) {
    Serial.print("JSON deserialization failed: ");
    Serial.println(error.c_str());
    return false;
  }
  strncpy(georev.name, doc[0]["name"], OW_GEOREV_STR_SIZE);
  strncpy(georev.state, doc[0]["state"], OW_GEOREV_STR_SIZE);
  strncpy(georev.country, doc[0]["country"], OW_GEOREV_STR_SIZE);
  Serial.print("Name: ");
  Serial.println(georev.name);
  Serial.print("State: ");
  Serial.println(georev.state);
  Serial.print("Country: ");
  Serial.println(georev.country);
  client.stop();
  return true;
}

bool updateExtra() {
  Serial.println("Calling weather API");
  WiFiClientSecure client;
  client.setInsecure();
  const char* host = "api.openweathermap.org";
  const uint16_t port = 443;
  if (!client.connect(host, port)) {
    Serial.println("Connection failed");
    return false;
  }
  client.print("GET ");
  client.print("/data/2.5/weather?lat=");
  client.print(latitude);
  client.print("&lon=");
  client.print(longitude);
  client.print("&units=");
  client.print(units);
  client.print("&lang=");
  client.print(lang);
  client.print("en&appid=");
  client.print(apiKey);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  client.print("\r\nConnection: close\r\n\r\n");
  if (client.println() == 0) {
    Serial.println("Failed to send request");
    return false;
  }
  Serial.println("Sent request, pulling out header");
  if (!client.find("\r\n\r\n")) {
    Serial.println("Could not find end of headers");
    return false;
  }
  Serial.println("Found end of header");
  Serial.println("Parsing JSON");
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, client);
  if (error) {
    Serial.print("JSON deserialization failed: ");
    Serial.println(error.c_str());
    return false;
  }
  extra.temp_min = doc["main"]["temp_min"];
  extra.temp_max = doc["main"]["temp_max"];
  extra.timezone = doc["timezone"];
  Serial.print("Minimum: ");
  Serial.println(extra.temp_min);
  Serial.print("Maximum: ");
  Serial.println(extra.temp_max);
  Serial.print("Timezone (second offset): ");
  Serial.println(extra.timezone);
  client.stop();
  return true;
}

bool updateWeather(bool useScreen) {
  Serial.println("Getting weather from OpenWeather");
  const bool success = ow.getForecast(&current, &hourly, &daily, apiKey,
                                      latitude, longitude, units, lang);
  if (success) {
    Serial.println("Obtained weather successfully!");
    display.println("Obtained weather successfully!");
  } else {
    Serial.println("Failed to get weather!");
    display.println("Failed to get weather!");
  }
  if (useScreen) {
    display.display();
    delay(5000);
  }
  return success;
}

// https://github.com/Bodmer/OpenWeather/blob/main/examples/Onecall%20API%20(subscription%20required)/My_OpenWeather_Test/My_OpenWeather_Test.ino#L95

String strTime(uint32_t unixTime) {
  return ctime((time_t*)&unixTime);
}

void printWeather() {
  Serial.println("Printing weather");
  Serial.println("Weather from Open Weather\n");

  Serial.print("Latitude            : ");
  Serial.println(ow.lat);
  Serial.print("Longitude           : ");
  Serial.println(ow.lon);
  Serial.print("Timezone            : ");
  Serial.println(ow.timezone);
  Serial.print("Name                : ");
  Serial.println(georev.name);
  Serial.print("State               : ");
  Serial.println(georev.state);
  Serial.print("Country             : ");
  Serial.println(georev.country);
  Serial.println();

  Serial.println("############### Current weather ###############\n");
  Serial.print("dt (time)        : ");
  Serial.println(strTime(current.dt));
  Serial.print("sunrise          : ");
  Serial.println(strTime(current.sunrise));
  Serial.print("sunset           : ");
  Serial.println(strTime(current.sunset));
  Serial.print("temp             : ");
  Serial.println(current.temp);
  Serial.print("feels_like       : ");
  Serial.println(current.feels_like);
  Serial.print("pressure         : ");
  Serial.println(current.pressure);
  Serial.print("humidity         : ");
  Serial.println(current.humidity);
  Serial.print("dew_point        : ");
  Serial.println(current.dew_point);
  Serial.print("uvi              : ");
  Serial.println(current.uvi);
  Serial.print("clouds           : ");
  Serial.println(current.clouds);
  Serial.print("visibility       : ");
  Serial.println(current.visibility);
  Serial.print("wind_speed       : ");
  Serial.println(current.wind_speed);
  Serial.print("wind_gust        : ");
  Serial.println(current.wind_gust);
  Serial.print("wind_deg         : ");
  Serial.println(current.wind_deg);
  Serial.print("rain             : ");
  Serial.println(current.rain);
  Serial.print("snow             : ");
  Serial.println(current.snow);
  Serial.println();
  Serial.print("id               : ");
  Serial.println(current.id);
  Serial.print("main             : ");
  Serial.println(current.main);
  Serial.print("description      : ");
  Serial.println(current.description);
  Serial.print("icon             : ");
  Serial.println(current.icon);

  Serial.println();

  Serial.println("############### Hourly weather  ###############\n");
  for (int i = 0; i < MAX_HOURS; i++) {
    Serial.print("Hourly summary  ");
    if (i < 10)
      Serial.print(" ");
    Serial.print(i);
    Serial.println();
    Serial.print("dt (time)        : ");
    Serial.println(strTime(hourly.dt[i]));
    Serial.print("temp             : ");
    Serial.println(hourly.temp[i]);
    Serial.print("feels_like       : ");
    Serial.println(hourly.feels_like[i]);
    Serial.print("pressure         : ");
    Serial.println(hourly.pressure[i]);
    Serial.print("humidity         : ");
    Serial.println(hourly.humidity[i]);
    Serial.print("dew_point        : ");
    Serial.println(hourly.dew_point[i]);
    Serial.print("clouds           : ");
    Serial.println(hourly.clouds[i]);
    Serial.print("wind_speed       : ");
    Serial.println(hourly.wind_speed[i]);
    Serial.print("wind_gust        : ");
    Serial.println(hourly.wind_gust[i]);
    Serial.print("wind_deg         : ");
    Serial.println(hourly.wind_deg[i]);
    Serial.print("rain             : ");
    Serial.println(hourly.rain[i]);
    Serial.print("snow             : ");
    Serial.println(hourly.snow[i]);
    Serial.println();
    Serial.print("id               : ");
    Serial.println(hourly.id[i]);
    Serial.print("main             : ");
    Serial.println(hourly.main[i]);
    Serial.print("description      : ");
    Serial.println(hourly.description[i]);
    Serial.print("icon             : ");
    Serial.println(hourly.icon[i]);
    Serial.print("pop              : ");
    Serial.println(hourly.pop[i]);

    Serial.println();
  }
  Serial.println("###############  Daily weather  ###############\n");
  for (int i = 0; i < MAX_DAYS; i++) {
    Serial.print("Daily summary   ");
    if (i < 10)
      Serial.print(" ");
    Serial.print(i);
    Serial.println();
    Serial.print("dt (time)        : ");
    Serial.println(strTime(daily.dt[i]));
    Serial.print("sunrise          : ");
    Serial.println(strTime(daily.sunrise[i]));
    Serial.print("sunset           : ");
    Serial.println(strTime(daily.sunset[i]));

    Serial.print("temp.morn        : ");
    Serial.println(daily.temp_morn[i]);
    Serial.print("temp.day         : ");
    Serial.println(daily.temp_day[i]);
    Serial.print("temp.eve         : ");
    Serial.println(daily.temp_eve[i]);
    Serial.print("temp.night       : ");
    Serial.println(daily.temp_night[i]);
    Serial.print("temp.min         : ");
    Serial.println(daily.temp_min[i]);
    Serial.print("temp.max         : ");
    Serial.println(daily.temp_max[i]);

    Serial.print("feels_like.morn  : ");
    Serial.println(daily.feels_like_morn[i]);
    Serial.print("feels_like.day   : ");
    Serial.println(daily.feels_like_day[i]);
    Serial.print("feels_like.eve   : ");
    Serial.println(daily.feels_like_eve[i]);
    Serial.print("feels_like.night : ");
    Serial.println(daily.feels_like_night[i]);

    Serial.print("pressure         : ");
    Serial.println(daily.pressure[i]);
    Serial.print("humidity         : ");
    Serial.println(daily.humidity[i]);
    Serial.print("dew_point        : ");
    Serial.println(daily.dew_point[i]);
    Serial.print("uvi              : ");
    Serial.println(daily.uvi[i]);
    Serial.print("clouds           : ");
    Serial.println(daily.clouds[i]);
    Serial.print("visibility       : ");
    Serial.println(daily.visibility[i]);
    Serial.print("wind_speed       : ");
    Serial.println(daily.wind_speed[i]);
    Serial.print("wind_gust        : ");
    Serial.println(daily.wind_gust[i]);
    Serial.print("wind_deg         : ");
    Serial.println(daily.wind_deg[i]);
    Serial.print("rain             : ");
    Serial.println(daily.rain[i]);
    Serial.print("snow             : ");
    Serial.println(daily.snow[i]);
    Serial.println();
    Serial.print("id               : ");
    Serial.println(daily.id[i]);
    Serial.print("main             : ");
    Serial.println(daily.main[i]);
    Serial.print("description      : ");
    Serial.println(daily.description[i]);
    Serial.print("icon             : ");
    Serial.println(daily.icon[i]);
    Serial.print("pop              : ");
    Serial.println(daily.pop[i]);

    Serial.println();
  }
}

uint16_t read16(fs::File& f) {
  // BMP data is stored little-endian, same as Arduino.
  uint16_t result;
  ((uint8_t*)&result)[0] = f.read(); // LSB
  ((uint8_t*)&result)[1] = f.read(); // MSB
  return result;
}

uint32_t read32(fs::File& f) {
  // BMP data is stored little-endian, same as Arduino.
  uint32_t result;
  ((uint8_t*)&result)[0] = f.read(); // LSB
  ((uint8_t*)&result)[1] = f.read();
  ((uint8_t*)&result)[2] = f.read();
  ((uint8_t*)&result)[3] = f.read(); // MSB
  return result;
}

// https://github.com/ZinggJM/GxEPD2/blob/master/examples/GxEPD2_Spiffs_Example/GxEPD2_Spiffs_Example.ino#L245
void drawBitmapFromSpiffs(const char* filename, int16_t x, int16_t y,
                          bool with_color = false) {
  static const uint16_t input_buffer_pixels = 800; // may affect performance

  static const uint16_t max_row_width =
      1872; // for up to 7.8" display 1872x1404
  static const uint16_t max_palette_pixels = 256; // for depth <= 8

  uint8_t input_buffer[3 * input_buffer_pixels]; // up to depth 24
  uint8_t output_row_mono_buffer[max_row_width /
                                 8]; // buffer for at least one row of b/w bits
  uint8_t output_row_color_buffer[max_row_width / 8]; // buffer for at least one
                                                      // row of color bits
  uint8_t mono_palette_buffer[max_palette_pixels /
                              8]; // palette buffer for depth <= 8 b/w
  uint8_t color_palette_buffer[max_palette_pixels /
                               8]; // palette buffer for depth <= 8 c/w

  fs::File file;
  bool valid = false; // valid format to be handled
  bool flip = true;   // bitmap is stored bottom-to-top
  uint32_t startTime = millis();
  if ((x >= display.epd2.WIDTH) || (y >= display.epd2.HEIGHT))
    return;
  Serial.println();
  Serial.print("Loading image '");
  Serial.print(filename);
  Serial.println('\'');
#if defined(ESP32)
  file = SPIFFS.open(filename, "r");
#else
  file = LittleFS.open(filename, "r");
#endif
  if (!file) {
    Serial.println("File not found");
    return;
  } else {
    Serial.println("Opened file successfully");
  }
  // Parse BMP header
  uint16_t signature = read16(file);
  Serial.print("Magic number: 0x");
  Serial.println(signature, HEX);
  if (signature == 0x4D42) // BMP signature
  {
    uint32_t fileSize = read32(file);
    uint32_t creatorBytes = read32(file);
    (void)creatorBytes;                  // unused
    uint32_t imageOffset = read32(file); // Start of image data
    uint32_t headerSize = read32(file);
    uint32_t width = read32(file);
    int32_t height = (int32_t)read32(file);
    uint16_t planes = read16(file);
    uint16_t depth = read16(file); // bits per pixel
    uint32_t format = read32(file);
    if ((planes == 1) &&
        ((format == 0) || (format == 3))) // uncompressed is handled, 565 also
    {
      Serial.print("File size: ");
      Serial.println(fileSize);
      Serial.print("Image Offset: ");
      Serial.println(imageOffset);
      Serial.print("Header size: ");
      Serial.println(headerSize);
      Serial.print("Bit Depth: ");
      Serial.println(depth);
      Serial.print("Image size: ");
      Serial.print(width);
      Serial.print('x');
      Serial.println(height);
      // BMP rows are padded (if needed) to 4-byte boundary
      uint32_t rowSize = (width * depth / 8 + 3) & ~3;
      if (depth < 8)
        rowSize = ((width * depth + 8 - depth) / 8 + 3) & ~3;
      if (height < 0) {
        height = -height;
        flip = false;
      }
      uint16_t w = width;
      uint16_t h = height;
      if ((x + w - 1) >= display.epd2.WIDTH)
        w = display.epd2.WIDTH - x;
      if ((y + h - 1) >= display.epd2.HEIGHT)
        h = display.epd2.HEIGHT - y;
      if (w <= max_row_width) // handle with direct drawing
      {
        valid = true;
        uint8_t bitmask = 0xFF;
        uint8_t bitshift = 8 - depth;
        uint16_t red, green, blue;
        bool whitish = false;
        bool colored = false;
        if (depth == 1)
          with_color = false;
        if (depth <= 8) {
          if (depth < 8)
            bitmask >>= depth;
          // file.seek(54); //palette is always @ 54
          file.seek(imageOffset -
                    (4 << depth)); // 54 for regular, diff for colorsimportant
          for (uint16_t pn = 0; pn < (1 << depth); pn++) {
            blue = file.read();
            green = file.read();
            red = file.read();
            file.read();
            whitish = with_color
                          ? ((red > 0x80) && (green > 0x80) && (blue > 0x80))
                          : ((red + green + blue) > 3 * 0x80); // whitish
            colored = (red > 0xF0) || ((green > 0xF0) &&
                                       (blue > 0xF0)); // reddish or yellowish?
            if (0 == pn % 8)
              mono_palette_buffer[pn / 8] = 0;
            mono_palette_buffer[pn / 8] |= whitish << pn % 8;
            if (0 == pn % 8)
              color_palette_buffer[pn / 8] = 0;
            color_palette_buffer[pn / 8] |= colored << pn % 8;
          }
        }
        uint32_t rowPosition =
            flip ? imageOffset + (height - h) * rowSize : imageOffset;
        for (uint16_t row = 0; row < h;
             row++, rowPosition += rowSize) // for each line
        {
          uint32_t in_remain = rowSize;
          uint32_t in_idx = 0;
          uint32_t in_bytes = 0;
          uint8_t in_byte = 0;           // for depth <= 8
          uint8_t in_bits = 0;           // for depth <= 8
          uint8_t out_byte = 0xFF;       // white (for w%8!=0 border)
          uint8_t out_color_byte = 0xFF; // white (for w%8!=0 border)
          uint32_t out_idx = 0;
          file.seek(rowPosition);
          for (uint16_t col = 0; col < w; col++) // for each pixel
          {
            // Time to read more pixel data?
            if (in_idx >= in_bytes) // ok, exact match for 24bit also (size IS
                                    // multiple of 3)
            {
              in_bytes =
                  file.read(input_buffer, in_remain > sizeof(input_buffer)
                                              ? sizeof(input_buffer)
                                              : in_remain);
              in_remain -= in_bytes;
              in_idx = 0;
            }
            switch (depth) {
              case 32:
                blue = input_buffer[in_idx++];
                green = input_buffer[in_idx++];
                red = input_buffer[in_idx++];
                in_idx++; // skip alpha
                whitish =
                    with_color
                        ? ((red > 0x80) && (green > 0x80) && (blue > 0x80))
                        : ((red + green + blue) > 3 * 0x80); // whitish
                colored =
                    (red > 0xF0) ||
                    ((green > 0xF0) && (blue > 0xF0)); // reddish or yellowish?
                break;
              case 24:
                blue = input_buffer[in_idx++];
                green = input_buffer[in_idx++];
                red = input_buffer[in_idx++];
                whitish =
                    with_color
                        ? ((red > 0x80) && (green > 0x80) && (blue > 0x80))
                        : ((red + green + blue) > 3 * 0x80); // whitish
                colored =
                    (red > 0xF0) ||
                    ((green > 0xF0) && (blue > 0xF0)); // reddish or yellowish?
                break;
              case 16: {
                uint8_t lsb = input_buffer[in_idx++];
                uint8_t msb = input_buffer[in_idx++];
                if (format == 0) // 555
                {
                  blue = (lsb & 0x1F) << 3;
                  green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2);
                  red = (msb & 0x7C) << 1;
                } else // 565
                {
                  blue = (lsb & 0x1F) << 3;
                  green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3);
                  red = (msb & 0xF8);
                }
                whitish =
                    with_color
                        ? ((red > 0x80) && (green > 0x80) && (blue > 0x80))
                        : ((red + green + blue) > 3 * 0x80); // whitish
                colored =
                    (red > 0xF0) ||
                    ((green > 0xF0) && (blue > 0xF0)); // reddish or yellowish?
              } break;
              case 1:
              case 2:
              case 4:
              case 8: {
                if (0 == in_bits) {
                  in_byte = input_buffer[in_idx++];
                  in_bits = 8;
                }
                uint16_t pn = (in_byte >> bitshift) & bitmask;
                whitish = mono_palette_buffer[pn / 8] & (0x1 << pn % 8);
                colored = color_palette_buffer[pn / 8] & (0x1 << pn % 8);
                in_byte <<= depth;
                in_bits -= depth;
              } break;
            }
            if (whitish) {
              // keep white
            } else if (colored && with_color) {
              out_color_byte &= ~(0x80 >> col % 8); // colored
            } else {
              out_byte &= ~(0x80 >> col % 8); // black
            }
            if ((7 == col % 8) ||
                (col == w - 1)) // write that last byte! (for w%8!=0 border)
            {
              output_row_color_buffer[out_idx] = out_color_byte;
              output_row_mono_buffer[out_idx++] = out_byte;
              out_byte = 0xFF;       // white (for w%8!=0 border)
              out_color_byte = 0xFF; // white (for w%8!=0 border)
            }
          } // end pixel
          uint16_t yrow = y + (flip ? h - row - 1 : row);
          // display.writeImage(output_row_mono_buffer, output_row_color_buffer,
          // x,
          //                    yrow, w, 1);
          display.drawBitmap(x, yrow, output_row_mono_buffer, w, 1,
                             GxEPD_BLACK);
        } // end line
        Serial.print("loaded in ");
        Serial.print(millis() - startTime);
        Serial.println(" ms");
      }
    } else {
      Serial.println("Invalid bitmap format and plane count");
      Serial.print("planes: ");
      Serial.println(planes);
      Serial.print("format: ");
      Serial.println(format);
    }
  } else {
    Serial.println("Not a bitmap");
  }
  file.close();
  if (!valid) {
    Serial.println("bitmap format not handled.");
  }
}

const char* getMeteoconIcon(uint16_t id, bool today = true) {
  if (today && id / 100 == 8 &&
      (current.dt < current.sunrise || current.dt > current.sunset))
    id += 1000;

  if (id / 100 == 2)
    return "thunderstorm";
  if (id / 100 == 3)
    return "drizzle";
  if (id / 100 == 4)
    return "unknown";
  if (id == 500)
    return "light-rain";
  else if (id == 511)
    return "sleet";
  else if (id / 100 == 5)
    return "rain";
  if (id >= 611 && id <= 616)
    return "sleet";
  else if (id / 100 == 6)
    return "snow";
  if (id / 100 == 7)
    return "fog";
  if (id == 800)
    return "clear-day";
  if (id == 801)
    return "partly-cloudy-day";
  if (id == 802)
    return "cloudy";
  if (id == 803)
    return "cloudy";
  if (id == 804)
    return "cloudy";
  if (id == 1800)
    return "clear-night";
  if (id == 1801)
    return "partly-cloudy-night";
  if (id == 1802)
    return "cloudy";
  if (id == 1803)
    return "cloudy";
  if (id == 1804)
    return "cloudy";

  return "unknown";
}

uint16_t getWidthOfText(const char* text) {
  int16_t tx, ty;
  uint16_t tw, th;
  display.getTextBounds(text, 0, 0, &tx, &ty, &tw, &th);
  return tw;
}

uint16_t printTemperature(float t, const char* unit, uint16_t x, uint16_t y) {
  uint16_t totalWidth = 0;
  const String temp = String(t, 0);
  display.setFont(&FreeMono18pt7b);
  display.setCursor(x, y);
  display.print(temp);
  uint16_t tw = getWidthOfText(temp.c_str());
  totalWidth += tw + 2;
  display.setFont(&FreeMono9pt7b);
  const uint16_t nx = x + tw + 6;
  const uint16_t ny = y - 10;
  display.setCursor(nx, ny);
  display.print(unit);
  tw = getWidthOfText(temp.c_str());
  totalWidth += tw + 4;
  return totalWidth;
}

void displayWeather() {
  Serial.println("Displaying weather");
  display.setTextColor(GxEPD_BLACK);
  display.fillScreen(GxEPD_WHITE);

  Serial.print("Heap: ");
  Serial.print(ESP.getFreeHeap() / 1024);
  Serial.println(" KiB");

  drawBitmapFromSpiffs(
      String("/icon/" + String(getMeteoconIcon(current.id)) + ".bmp").c_str(),
      2, 2);

  display.setFont(&FreeMono9pt7b);
  display.setCursor(103, 21);
  display.print(georev.name);
  display.print(", ");
  display.print(georev.state);
  display.print(", ");
  display.print(georev.country);

  display.setFont(&FreeMono24pt7b);
  display.setCursor(100, 53);
  display.print(current.main);

  uint16_t currX = 100;
  currX += printTemperature(
      current.temp, strcmp(units, "imperial") == 0 ? "oF" : "oC", currX, 84);
  display.setFont(&FreeMono18pt7b);
  display.setCursor(currX, 84);
  display.print(" (");
  currX += getWidthOfText(" (");
  currX += printTemperature(
      extra.temp_min, strcmp(units, "imperial") == 0 ? "oF" : "oC", currX, 84);
  display.setFont(&FreeMono18pt7b);
  display.setCursor(currX, 84);
  display.print("-");
  currX += getWidthOfText("-") + 2;
  currX += printTemperature(
      extra.temp_max, strcmp(units, "imperial") == 0 ? "oF" : "oC", currX, 84);
  display.setFont(&FreeMono18pt7b);
  display.setCursor(currX, 84);
  display.print(")");
  currX += getWidthOfText(")");

  currX = 2;
  display.setFont(&FreeMono12pt7b);
  display.setCursor(currX, 114);
  display.print("Humidity: ");
  display.print(current.humidity);
  display.print("%");

  display.setCursor(currX, 136);
  display.print("Wind: ");
  display.print(round(current.wind_speed), 0);
  if (strcmp(units, "imperial") == 0) {
    display.print(" mph ");
  } else {
    display.print(" m/s ");
  }
  uint16_t windAngle = (current.wind_deg + 22.5) / 45;
  if (windAngle > 7) {
    windAngle = 0;
  }
  const char* windText[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  display.print(windText[windAngle]);

  display.setCursor(currX, 158);
  display.print("UV index: ");
  display.print(round(current.uvi), 0);
  // https://www.epa.gov/sunsafety/uv-index-scale-0
  if (current.uvi >= 11) {
    display.print(" (extreme)");
  } else if (current.uvi >= 8) {
    display.print(" (very high)");
  } else if (current.uvi >= 6) {
    display.print(" (high)");
  } else if (current.uvi >= 3) {
    display.print(" (moderate)");
  } else {
    display.print(" (low)");
  }

  display.setFont(&FreeMono12pt7b);
  const uint8_t charWidth = getWidthOfText("-");
  uint16_t x = charWidth * 2.5;
  uint16_t y = 172;
  for (uint8_t i = 1; i < MAX_DAYS; i++) {
    Serial.print("Forecast for ");
    Serial.println(daysOfTheWeek[weekday(daily.dt[i])]);
    Serial.print("Min: ");
    Serial.println(round(daily.temp_min[i]), 0);
    Serial.print("Max: ");
    Serial.println(round(daily.temp_max[i]), 0);
    display.setCursor(x, y + 14);
    display.print(" ");
    display.print(daysOfTheWeek[weekday(daily.dt[i])]);
    display.print(" ");
    display.setCursor(x, y + 36);
    display.print(round(daily.temp_min[i]), 0);
    display.print(" ");
    display.print(round(daily.temp_max[i]), 0);
    drawBitmapFromSpiffs(
        String("/icon50/" + String(getMeteoconIcon(daily.id[i])) + ".bmp")
            .c_str(),
        x + charWidth, y + 34);
    x += getWidthOfText(" Sun   ") + 2;
  }

  display.display();

  Serial.print("Heap: ");
  Serial.print(ESP.getFreeHeap() / 1024);
  Serial.println(" KiB");
}

void setup() {
  const uint32_t cycleStart = millis();

  Serial.begin(SERIAL_SPEED);
  Serial.println();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  delay(100);

  userBtn.begin();

  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS failed");
  }

  display.init(SERIAL_SPEED, true, 2, false);
  display.setRotation(0);
  display.setFont(&FreeMono9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);

  bool showBootup = true;

  printWakeupReason();
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
      lastUpdateSuccess) {
    showBootup = false;
  }

  if (showBootup) {
    Serial.println("Showing bootup text");
    display.display();
  } else {
    Serial.println("Not showing bootup text");
  }

#ifdef FAST_BOOT
  Serial.println("Fast bootup enabled, not showing bootup text");
  showBootup = false;
#endif

  esp_sleep_enable_ext0_wakeup(USER_BTN_RTC_PIN, 0);

  connectToWiFi(showBootup);
  updateTime();
  if (strlen(georev.name) == 0) {
    Serial.println("Determined name from coordinates empty, calling reverse "
                   "geocoding API");
    updateGeocodingReverse();
  }
  updateExtra();
  updateWeather(showBootup);
  printWeather();
  disconnectFromWiFi();
  displayWeather();

  const uint32_t cycleEnd = millis();
  const uint32_t cycleTime = cycleEnd - cycleStart;

  Serial.print("Cycle took ");
  Serial.print(cycleTime / 1000.0);
  Serial.println(" seconds");

  lastUpdateSuccess = true;
  Serial.print("Updating again in ");
  Serial.print(UPDATE_TIME);
  Serial.println(" minute...");
  delay(1000);
  Serial.print("Deep sleeping for ");
  Serial.print(UPDATE_TIME);
  Serial.println(" minutes");
  ESP.deepSleep(UPDATE_TIME * 60 * 1000000);

somethingFailed:
  lastUpdateSuccess = false;
  Serial.print("Trying again in ");
  Serial.print(FAIL_RETRY_TIME);
  Serial.println(" minute...");
  display.print("Trying again in ");
  display.print(FAIL_RETRY_TIME);
  display.println(" minute...");
  display.display();
  delay(1000);
  Serial.print("Deep sleeping for ");
  Serial.print(FAIL_RETRY_TIME);
  Serial.println(" minutes");
  ESP.deepSleep(FAIL_RETRY_TIME * 60 * 1000000);
}

void loop() {}
