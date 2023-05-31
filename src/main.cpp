#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Button.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_display_selection_new_style.h>
#include <WiFi.h>
#include <WiFiManager.h>

const uint8_t USER_BTN_PIN = 27;

const char* CONFIG_AP_NAME = "WeatherStationConfig";

Button userBtn(USER_BTN_PIN);

bool connectToWiFi(bool useScreen) {
  bool startedConfigAP = false;
  bool displayedAboutStartedConfigAP = false;
  bool configAPTimedOut = false;
  bool displayedAboutConfigAPTimedOut = false;

  WiFiManager wm;
  WiFi.mode(WIFI_STA);
  wm.setAPCallback(
      [&startedConfigAP](WiFiManager* wm) { startedConfigAP = true; });
  wm.setConfigPortalTimeoutCallback(
      [&configAPTimedOut]() { configAPTimedOut = true; });
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
        Serial.println("\" and open http://192.168.4.1 to open the WiFi credential configuration page.");
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

void setup() {
  Serial.begin(9600);
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
  display.display();

  connectToWiFi(true);
}

void loop() {}
