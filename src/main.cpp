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
  // updateWeather(showBootup);

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
