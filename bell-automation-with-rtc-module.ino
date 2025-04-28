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
#define EEPROM_SIZE 512  
#define RESET_BUTTON_PIN D0  
#define BELL_PIN D3  
#define SDA_PIN D2   
#define SCL_PIN D1   

// EEPROM address offsets
#define WIFI_SSID_ADDR 0       
#define WIFI_PASS_ADDR 32     
#define BELL_ENABLED_ADDR 96 
#define BELL_SCHEDULE_ADDR 100 


#define MAX_BELLS 9


#define RELAY_ON LOW
#define RELAY_OFF HIGH


#define API_KEY "AIzaSyAujt_zf6fCCLEPICef4_VAd7W4rSQshJE"
#define DATABASE_URL "byte4genodemcu-default-rtdb.firebaseio.com"

// Bell schedule structure
struct BellSchedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t state;  // 0 = not rung yet, 1 = already rung
};

// Global bell schedule array
BellSchedule bellSchedules[MAX_BELLS];
bool bellEnabled = false;

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
  for (int i = 0; i < 32; i++) EEPROM.write(WIFI_SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
  for (int i = 0; i < 64; i++) EEPROM.write(WIFI_PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  EEPROM.commit();
}

void loadWiFiCredentials(char* ssid, char* pass) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 32; i++) ssid[i] = EEPROM.read(WIFI_SSID_ADDR + i);
  ssid[32] = '\0';
  for (int i = 0; i < 64; i++) pass[i] = EEPROM.read(WIFI_PASS_ADDR + i);
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

// Function to reset WiFi credentials 
void resetWiFiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Reset SSID and password in EEPROM to empty values
  for (int i = 0; i < 32; i++) EEPROM.write(WIFI_SSID_ADDR + i, 0);  // Clear SSID
  for (int i = 0; i < 64; i++) EEPROM.write(WIFI_PASS_ADDR + i, 0);  // Clear password
  
  EEPROM.commit();  // Save changes to EEPROM
  Serial.println("Wi-Fi credentials reset!");
  
  // Alert user that credentials were reset
  for (int i = 0; i < 3; i++) {
    digitalWrite(BELL_PIN, RELAY_ON);  // Turn relay ON
    delay(300);
    digitalWrite(BELL_PIN, RELAY_OFF); // Turn relay OFF
    delay(300);
  }
  
  // Wait a moment, then restart
  delay(1000);
  ESP.restart();
}

// Function to check if the reset button is pressed
void checkResetButton() {
  static unsigned long lastDebounceTime = 0;
  static bool lastButtonState = HIGH;
  static unsigned long buttonPressStartTime = 0;
  
  // Read the current state of the reset button
  bool buttonState = digitalRead(RESET_BUTTON_PIN);
  
  // Check if the button state has changed
  if (buttonState != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  // If the button state has been stable for the debounce delay
  if ((millis() - lastDebounceTime) > 50) {
    // If the button is pressed (LOW due to INPUT_PULLUP)
    if (buttonState == LOW) {
      // If this is the start of a button press, record the time
      if (lastButtonState == HIGH) {
        buttonPressStartTime = millis();
      }
      
      // If the button has been held for more than 5 seconds
      if ((millis() - buttonPressStartTime) > 5000) {
        Serial.println("Reset button held for 5 seconds - resetting WiFi credentials");
        resetWiFiCredentials();
      }
    }
  }
  
  // Save the current button state for the next comparison
  lastButtonState = buttonState;
}

// Save bell schedules to EEPROM
void saveBellSchedulesToEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Save bell enabled state
  EEPROM.write(BELL_ENABLED_ADDR, bellEnabled ? 1 : 0);
  
  // Save bell schedules
  for (int i = 0; i < MAX_BELLS; i++) {
    int addr = BELL_SCHEDULE_ADDR + (i * 3);  // Each schedule takes 3 bytes
    EEPROM.write(addr, bellSchedules[i].hour);
    EEPROM.write(addr + 1, bellSchedules[i].minute);
    EEPROM.write(addr + 2, 0);  // Always reset state to 0 when saving
  }
  
  EEPROM.commit();
  Serial.println("Bell schedules saved to EEPROM");
}

// Load bell schedules from EEPROM
void loadBellSchedulesFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Load bell enabled state
  bellEnabled = EEPROM.read(BELL_ENABLED_ADDR) == 1;
  
  // Load bell schedules
  for (int i = 0; i < MAX_BELLS; i++) {
    int addr = BELL_SCHEDULE_ADDR + (i * 3);
    bellSchedules[i].hour = EEPROM.read(addr);
    bellSchedules[i].minute = EEPROM.read(addr + 1);
    bellSchedules[i].state = EEPROM.read(addr + 2);
  }
  
  Serial.println("Bell schedules loaded from EEPROM");
  
  // Print loaded schedules
  Serial.println("Bell status: " + String(bellEnabled ? "Enabled" : "Disabled"));
  for (int i = 0; i < MAX_BELLS; i++) {
    Serial.print("Bell #");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(bellSchedules[i].hour);
    Serial.print(":");
    Serial.print(bellSchedules[i].minute);
    Serial.print(" (State: ");
    Serial.print(bellSchedules[i].state);
    Serial.println(")");
  }
}

// Update a specific bell state in EEPROM
void updateBellStateInEEPROM(int bellIndex, uint8_t state) {
  if (bellIndex < 0 || bellIndex >= MAX_BELLS) return;
  
  EEPROM.begin(EEPROM_SIZE);
  int addr = BELL_SCHEDULE_ADDR + (bellIndex * 3) + 2;  // Address of state byte
  EEPROM.write(addr, state);
  EEPROM.commit();
  
  // Update in-memory state too
  bellSchedules[bellIndex].state = state;
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

// Function to check and ring bells when online (Firebase available)
void checkAndRingBellsOnline() {
  if (Firebase.ready() && signupOK) {
    // Check overall bell system status
    if (Firebase.RTDB.getInt(&fbdo, "/SchoolBell/status")) {
      int status = fbdo.intData();
      bellEnabled = (status == 1);
      
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
        bool schedulesUpdated = false;

        for (int i = 1; i <= MAX_BELLS; i++) {
          int bellIndex = i - 1;  // Convert to 0-based index for our array
          String pathHour = "/SchoolBell/" + String(i) + "/h";
          String pathMinute = "/SchoolBell/" + String(i) + "/m";
          String pathBellState = "/SchoolBell/" + String(i) + "/state";

          String bellHour, bellMinute, bellState;

          // Retrieve the Bell Hour from Firebase
          if (Firebase.RTDB.getString(&fbdo, pathHour)) {
            bellHour = fbdo.stringData();
            bellSchedules[bellIndex].hour = bellHour.toInt();
            schedulesUpdated = true;
          } else {
            Serial.println("Failed to get Bell Hour: " + fbdo.errorReason());
            continue; // Skip to the next iteration
          }

          // Retrieve the Bell Minute from Firebase
          if (Firebase.RTDB.getString(&fbdo, pathMinute)) {
            bellMinute = fbdo.stringData();
            bellSchedules[bellIndex].minute = bellMinute.toInt();
            schedulesUpdated = true;
          } else {
            Serial.println("Failed to get Bell Minute: " + fbdo.errorReason());
            continue; // Skip to the next iteration
          }

          // Retrieve the Bell State from Firebase
          if (Firebase.RTDB.getString(&fbdo, pathBellState)) {
            bellState = fbdo.stringData();
            bellSchedules[bellIndex].state = bellState.toInt();
          } else {
            Serial.println("Failed to get Bell State: " + fbdo.errorReason());
            continue; // Skip to the next iteration
          }

          // Convert to integers
          int bellHourInt = bellSchedules[bellIndex].hour;
          int bellMinuteInt = bellSchedules[bellIndex].minute;
          int bellStateInt = bellSchedules[bellIndex].state;

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
              
              // Update local state and EEPROM
              bellSchedules[bellIndex].state = 1;
              updateBellStateInEEPROM(bellIndex, 1);
              
              delay(60000); // Wait a minute before resetting state

              // Reset status back to 0 in Firebase Realtime Database
              if (Firebase.RTDB.setInt(&fbdo, "/SchoolBell/" + String(i) + "/state", 0)) {
                Serial.println("Status updated to 0 successfully!");
              } else {
                Serial.println("Failed to update status to 0");
              }
              
              // Update local state and EEPROM
              bellSchedules[bellIndex].state = 0;
              updateBellStateInEEPROM(bellIndex, 0);

              bellRinging = true;
              break; // Stop checking further times once a match is found
            } else {
              Serial.println("Already done.");
            }
          } else {
            Serial.println("No Bell Scheduled for " + String(i));
          }
        }

        // If schedules were updated from Firebase, save them to EEPROM
        if (schedulesUpdated) {
          saveBellSchedulesToEEPROM();
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

// Function to check and ring bells when offline (using EEPROM saved schedules)
void checkAndRingBellsOffline() {
  // Get current time from RTC
  DateTime now = rtc.now();
  int currentHour = now.hour();
  int currentMinute = now.minute();
  
  Serial.print("Offline Mode - Current Time: ");
  Serial.print(currentHour);
  Serial.print(":");
  Serial.println(currentMinute);
  
  // Check if bell system is enabled according to EEPROM
  if (bellEnabled) {
    bool bellRinging = false;
    
    for (int i = 0; i < MAX_BELLS; i++) {
      // Compare current time with Bell time
      if ((currentHour == bellSchedules[i].hour) && (currentMinute == bellSchedules[i].minute)) {
        if (bellSchedules[i].state == 0) {
          Serial.print("Offline Mode - Ringing bell #");
          Serial.println(i + 1);
          
          ringBell(i + 1);
          
          // Update local state
          bellSchedules[i].state = 1;
          updateBellStateInEEPROM(i, 1);
          
          delay(60000); // Wait a minute before being eligible for reset
          
          // Update local state
          bellSchedules[i].state = 0;
          updateBellStateInEEPROM(i, 0);
          
          bellRinging = true;
          break; // Stop checking further times once a match is found
        } else {
          Serial.println("Offline Mode - Bell already rung.");
        }
      }
    }
    
    if (!bellRinging) {
      digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF when not ringing
    }
  } else {
    Serial.println("Offline Mode - Bell system is disabled");
    digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF when disabled
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize the bell pin as OUTPUT and ensure it's OFF by default
  pinMode(BELL_PIN, OUTPUT);
  digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF during initialization
  
  // Initialize reset button pin with internal pull-up resistor
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  
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
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/networks", handleNetworks);

  // Double-check that relay is still OFF before continuing
  digitalWrite(BELL_PIN, RELAY_OFF);
  
  // Load bell schedules from EEPROM (to be used if WiFi is not connected)
  loadBellSchedulesFromEEPROM();
  
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
  
  // Check if reset button is pressed
  checkResetButton();
  
  // Check if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    // If online, check bells using Firebase data and update EEPROM
    checkAndRingBellsOnline();
  } else {
    // If offline, check bells using saved schedules from EEPROM
    Serial.println("WiFi not connected - using saved bell schedules from EEPROM");
    checkAndRingBellsOffline();
    digitalWrite(BELL_PIN, RELAY_OFF); // Ensure relay is OFF if WiFi is disconnected
  }
  
  delay(100);
}
