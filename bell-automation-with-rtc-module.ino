#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <DNSServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

DNSServer dnsServer;
const byte DNS_PORT = 53;

// ---------- Config ----------
#define EEPROM_SIZE 96
#define RESET_BUTTON_PIN 32  // Define the reset button pin
#define BELL_PIN D3  // Relay pin for the bell (changed from D1)
#define SDA_PIN D2   // SDA pin for DS3231 (GPIO4)
#define SCL_PIN D1   // SCL pin for DS3231 (GPIO5)

// Relay states (relay is active-low, meaning LOW turns it ON and HIGH turns it OFF)
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// Firebase config
#define API_KEY "AIzaSyAujt_zf6fCCLEPICef4_VAd7W4rSQshJE"
#define DATABASE_URL "byte4genodemcu-default-rtdb.firebaseio.com"

// ---------- Objects ----------
ESP8266WebServer server(80);
WiFiClient espClient;

// RTC DS3231 setup
RTC_DS3231 rtc;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// ---------- EEPROM ----------
void saveWiFiCredentials(String ssid, String pass) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 32; i++) EEPROM.write(i, i < ssid.length() ? ssid[i] : 0);
  for (int i = 0; i < 64; i++) EEPROM.write(32 + i, i < pass.length() ? pass[i] : 0);
  EEPROM.commit();
}

void loadWiFiCredentials(char* ssid, char* pass) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 32; i++) ssid[i] = EEPROM.read(i);
  ssid[32] = '\0';
  for (int i = 0; i < 64; i++) pass[i] = EEPROM.read(32 + i);
  pass[64] = '\0';
}

bool connectToWiFiFromEEPROM() {
  char ssid[33] = {0};
  char pass[65] = {0};
  loadWiFiCredentials(ssid, pass);
  if (strlen(ssid) == 0) return false;

  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

void resetWiFiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Reset SSID and password in EEPROM to empty values
  for (int i = 0; i < 32; i++) EEPROM.write(i, 0);  // Clear SSID
  for (int i = 0; i < 64; i++) EEPROM.write(32 + i, 0);  // Clear password
  
  EEPROM.commit();  // Save changes to EEPROM
  Serial.println("Wi-Fi credentials reset!");
}

// ---------- Web Config ----------
void handleRoot() {
  String html = R"====(
  <!DOCTYPE html><html lang='en'><head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>BYTE4GE SMART HOME</title>
    <style>
      body {
        background-color: #121212; color: #fff; font-family: Arial, sans-serif;
        margin: 0; padding: 0; display: flex; justify-content: center;
        align-items: center; height: 100vh; flex-direction: column;
      }
      h2 { color: #f39c12; font-size: 2rem; margin-bottom: 20px; }
       p { color: #f39c12;}
      form {
        background-color: #333; padding: 20px; border-radius: 8px;
        box-shadow: 0 4px 8px rgba(0,0,0,0.2); width: 100%; max-width: 400px;
        margin-top: 20px;
      }
      label { display: block; margin-bottom: 8px; }
      select, input[type='password'] {
        background-color: #444; border: none; color: #fff;
        padding: 10px; border-radius: 4px; width: 100%;
        margin-bottom: 16px; font-size: 1rem;
      }
      input[type='submit'] {
        background-color: #f39c12; color: #fff; border: none;
        padding: 10px 20px; border-radius: 4px; cursor: pointer;
        width: 100%; font-size: 1rem; margin-top: 20px;
      }
      .loader {
        border: 4px solid #f3f3f3;
        border-top: 4px solid #3498db;
        border-radius: 50%;
        width: 40px;
        height: 40px;
        animation: spin 1.5s linear infinite;
        margin: 20px auto;
      }
    </style>
  </head><body>
    <h2>CONNECT TO WIFI</h2>
    <form action='/save' method='post' onsubmit='showLoader()'>
      <label for='ssid'>Select Network:</label>
      <select name='ssid' id='ssid'><option>Loading...</option></select>
      <label for='pass'>Password:</label>
      <input name='pass' type='password'>
      <input type='submit' value='Save & Reboot'>
    </form>
    <p>Copyright Ⓒ 2023-24 Byte4ge (Sardar Enterprises) All Rights Reserved.</p>
    <script>
      async function loadNetworks() {
        const res = await fetch('/networks');
        const networks = await res.json();
        const select = document.getElementById('ssid');
        select.innerHTML = '';
        networks.forEach(ssid => {
          const opt = document.createElement('option');
          opt.value = ssid;
          opt.textContent = ssid;
          select.appendChild(opt);
        });
      }

      loadNetworks();
    </script>
  </body></html>
  )====";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    // Save credentials to EEPROM (optional)
    saveWiFiCredentials(ssid, pass);
    
    // Attempt to connect to the chosen Wi-Fi network
    WiFi.begin(ssid.c_str(), pass.c_str());

    // Wait for connection
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to Wi-Fi!");
      server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      background-color: #121212;
      color: #ffffff;
      font-family: 'Segoe UI', sans-serif;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      height: 100vh;
      margin: 0;
      padding: 0;
    }

    .message-box {
      background-color: #1f1f1f;
      padding: 30px;
      border-radius: 12px;
      box-shadow: 0 0 10px rgba(255, 255, 255, 0.1);
      text-align: center;
    }

    .message-box h3 {
      margin-bottom: 20px;
    }

    .close-btn {
      background-color: #ff4c4c;
      color: white;
      border: none;
      padding: 10px 20px;
      border-radius: 8px;
      font-size: 16px;
      cursor: pointer;
    }

    .close-btn:hover {
      background-color: #ff1c1c;
    }

    @media (max-width: 600px) {
      .message-box {
        width: 90%;
      }
    }
  </style>
  <script>
    setTimeout(() => window.close(), 5000);
  </script>
</head>
<body>
  <div class="message-box">
    <h3>🎉 Connected! Rebooting...</h3>
    <button class="close-btn" onclick="window.close()">Close</button>
  </div>
</body>
</html>
)rawliteral");

      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/html", "<h3>Failed to connect to Wi-Fi!</h3>");
    }
  } else {
    server.send(400, "text/plain", "Missing SSID or Password");
  }
}

void handleNetworks() {
  int n = WiFi.scanNetworks();
  String json = "[";

  for (int i = 0; i < n; i++) {
    json += "\"" + WiFi.SSID(i) + "\"";
    if (i < n - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void startAPMode() {
  String apSSID = "Auto Bell";
  WiFi.softAP(apSSID.c_str());

  IPAddress IP = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", IP);

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/networks", handleNetworks);
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Wi-Fi Config Mode. Connect to: " + apSSID);
  Serial.println("Open: http://" + IP.toString());
}

// ---------- Bell Functions ----------
void ringBell(int bellNumber) {
  Serial.println("Bell ringing started for bell #" + String(bellNumber));
  
  if (bellNumber == 1 || bellNumber == 6) {
    // Long ring followed by specific number of short rings
    digitalWrite(BELL_PIN, RELAY_ON);  // Turn relay ON
    delay(5000);
    digitalWrite(BELL_PIN, RELAY_OFF); // Turn relay OFF
    delay(1000);
    
    for (int j = 0; j < bellNumber; j++) {
      digitalWrite(BELL_PIN, RELAY_ON);  // Turn relay ON
      delay(1000);
      digitalWrite(BELL_PIN, RELAY_OFF); // Turn relay OFF
      delay(1000);
    }
  }
  else if (bellNumber == 5 || bellNumber == 9) {
    // Just a long ring
    digitalWrite(BELL_PIN, RELAY_ON);  // Turn relay ON
    delay(5000);
    digitalWrite(BELL_PIN, RELAY_OFF); // Turn relay OFF
  }
  else {
    // Blink the LED bellNumber times with standard timing
    for (int j = 0; j < bellNumber; j++) {
      digitalWrite(BELL_PIN, RELAY_ON);  // Turn relay ON
      delay(1000);
      digitalWrite(BELL_PIN, RELAY_OFF); // Turn relay OFF
      delay(1000);
    }
  }
}

void checkAndRingBells() {
  if (Firebase.ready() && signupOK) {
    if (Firebase.RTDB.getInt(&fbdo, "/SchoolBell/status")) {
      int status = fbdo.intData();
      if (status == 1) {
        // Get current time from RTC
        DateTime now = rtc.now();
        int currentHour = now.hour();
        int currentMinute = now.minute();
        
        Serial.print("Current Time: ");
        Serial.print(currentHour);
        Serial.print(":");
        Serial.println(currentMinute);

        bool bellRinging = false;

        for (int i = 1; i <= 9; i++) {
          String pathHour = "/SchoolBell/" + String(i) + "/h";
          String pathMinute = "/SchoolBell/" + String(i) + "/m";
          String pathBellState = "/SchoolBell/" + String(i) + "/state";

          String bellHour, bellMinute, bellState;

          // Retrieve the Bell Hour from Firebase
          if (Firebase.RTDB.getString(&fbdo, pathHour)) {
            bellHour = fbdo.stringData();
          } else {
            Serial.println("Failed to get Bell Hour: " + fbdo.errorReason());
            continue; // Skip to the next iteration
          }

          // Retrieve the Bell Minute from Firebase
          if (Firebase.RTDB.getString(&fbdo, pathMinute)) {
            bellMinute = fbdo.stringData();
          } else {
            Serial.println("Failed to get Bell Minute: " + fbdo.errorReason());
            continue; // Skip to the next iteration
          }

          // Retrieve the Bell State from Firebase
          if (Firebase.RTDB.getString(&fbdo, pathBellState)) {
            bellState = fbdo.stringData();
          } else {
            Serial.println("Failed to get Bell State: " + fbdo.errorReason());
            continue; // Skip to the next iteration
          }

          // Convert to integers
          int bellHourInt = bellHour.toInt();
          int bellMinuteInt = bellMinute.toInt();
          int bellStateInt = bellState.toInt();

          // Compare current time with Bell time
          if ((currentHour == bellHourInt) && (currentMinute == bellMinuteInt)) {
            if (bellStateInt == 0) {
              ringBell(i);

              // Update status to 1 in Firebase Realtime Database
              if (Firebase.RTDB.setInt(&fbdo, "/SchoolBell/" + String(i) + "/state", 1)) {
                Serial.println("Status updated to 1 successfully!");
              } else {
                Serial.println("Failed to update status to 1");
              }
              
              delay(60000); // Wait a minute before resetting state

              // Reset status back to 0 in Firebase Realtime Database
              if (Firebase.RTDB.setInt(&fbdo, "/SchoolBell/" + String(i) + "/state", 0)) {
                Serial.println("Status updated to 0 successfully!");
              } else {
                Serial.println("Failed to update status to 0");
              }

              bellRinging = true;
              break; // Stop checking further times once a match is found
            } else {
              Serial.println("Already done.");
            }
          } else {
            Serial.println("No Bell Scheduled for " + String(i));
          }
        }

        if (!bellRinging) {
          digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF when not ringing
        }
      } else {
        Serial.println("Bell status is off");
        digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF when bell status is disabled
      }
    } else {
      Serial.println("Failed to get Bell status from Firebase");
      digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF if we can't get Firebase status
    }
  } else {
    Serial.println("Firebase is not ready");
    digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF if Firebase is not ready
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize the bell pin as OUTPUT and ensure it's OFF by default
  pinMode(BELL_PIN, OUTPUT);
  digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF during initialization
  
  // Initialize I2C communication for the DS3231
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize the RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC! Check wiring.");
    while (1);
  }
  
  // Uncomment the line below to set the RTC to the date & time this sketch was compiled
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting to compile time!");
    // Set the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/networks", handleNetworks);

  // Double-check that relay is still OFF before continuing
  digitalWrite(BELL_PIN, RELAY_OFF);
  
  if (!connectToWiFiFromEEPROM()) {
    startAPMode();
  } else {
    Serial.println("Connected to Wi-Fi");
    
    // Configure Firebase
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    
    // Firebase sign-up
    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase sign-up successful");
      signupOK = true;
    } else {
      Serial.printf("Firebase sign-up failed: %s\n", config.signer.signupError.message.c_str());
    }
    
    // Set token status callback
    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    
    // Display current time from RTC
    DateTime now = rtc.now();
    Serial.print("RTC Time: ");
    Serial.print(now.year(), DEC);
    Serial.print('/');
    Serial.print(now.month(), DEC);
    Serial.print('/');
    Serial.print(now.day(), DEC);
    Serial.print(" ");
    Serial.print(now.hour(), DEC);
    Serial.print(':');
    Serial.print(now.minute(), DEC);
    Serial.print(':');
    Serial.print(now.second(), DEC);
    Serial.println();
    
    // Start web server
    server.begin();
    
    // Ensure relay is still OFF after all initialization
    digitalWrite(BELL_PIN, RELAY_OFF);
  }
}

void loop() {
  server.handleClient();
  dnsServer.processNextRequest();
  
  // Only check bells if we're connected to WiFi
  if (WiFi.status() == WL_CONNECTED) {
    checkAndRingBells();
  } else {
    digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF if WiFi is disconnected
  }
  
  delay(100);
}
