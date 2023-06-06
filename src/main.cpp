#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Button.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_display_selection_new_style.h>
#include <OpenWeather.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>

const uint8_t USER_BTN_PIN = 27;
const gpio_num_t USER_BTN_RTC_PIN = GPIO_NUM_27;

const char* CONFIG_AP_NAME = "WeatherStationConfig";

const uint32_t FAIL_RETRY_TIME = 1;
const uint32_t UPDATE_TIME = 5;

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

bool updateWeather(bool useScreen) {
  Serial.println("Getting weather from OpenWeather");
  bool success = ow.getForecast(&current, &hourly, &daily, apiKey, latitude,
                                longitude, units, lang);
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

void setup() {
  Serial.begin(9600);
  Serial.println();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  delay(100);

  userBtn.begin();

  display.init(9600, true, 2, false);
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

  esp_sleep_enable_ext0_wakeup(USER_BTN_RTC_PIN, 0);

  connectToWiFi(showBootup);
  updateWeather(showBootup);
  printWeather();

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
