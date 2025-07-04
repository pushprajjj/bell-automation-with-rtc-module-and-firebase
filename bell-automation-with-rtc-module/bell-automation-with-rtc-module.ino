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


#define EEPROM_SIZE 512  
#define RESET_BUTTON_PIN D0  
#define BELL_PIN D3  
#define SDA_PIN D2   
#define SCL_PIN D1   


#define WIFI_SSID_ADDR 0       
#define WIFI_PASS_ADDR 32     
#define BELL_ENABLED_ADDR 96 
#define BELL_SCHEDULE_ADDR 100 


#define MAX_BELLS 9


#define RELAY_ON LOW
#define RELAY_OFF HIGH


#define API_KEY "AIzaSyAujt_zf6fCCLEPICef4_VAd7W4rSQshJE"
#define DATABASE_URL "byte4genodemcu-default-rtdb.firebaseio.com"


struct BellSchedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t state;  
};


BellSchedule bellSchedules[MAX_BELLS];
bool bellEnabled = false;


ESP8266WebServer server(80);
WiFiClient espClient;


RTC_DS3231 rtc;


FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;


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


void resetWiFiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  

  for (int i = 0; i < 32; i++) EEPROM.write(WIFI_SSID_ADDR + i, 0);  
  for (int i = 0; i < 64; i++) EEPROM.write(WIFI_PASS_ADDR + i, 0);  
  
  EEPROM.commit();  
  Serial.println("Wi-Fi credentials reset!");
  
  // Alert user that credentials were reset
  for (int i = 0; i < 3; i++) {
    digitalWrite(BELL_PIN, RELAY_ON);  
    delay(300);
    digitalWrite(BELL_PIN, RELAY_OFF); 
    delay(300);
  }
  
  
  delay(1000);
  ESP.restart();
}


void checkResetButton() {
  static unsigned long lastDebounceTime = 0;
  static bool lastButtonState = HIGH;
  static unsigned long buttonPressStartTime = 0;
  

  bool buttonState = digitalRead(RESET_BUTTON_PIN);
  
  
  if (buttonState != lastButtonState) {
    lastDebounceTime = millis();
  }
  

  if ((millis() - lastDebounceTime) > 50) {
  
    if (buttonState == LOW) {
    
      if (lastButtonState == HIGH) {
        buttonPressStartTime = millis();
      }
      
  
      if ((millis() - buttonPressStartTime) > 5000) {
        Serial.println("Reset button held for 5 seconds - resetting WiFi credentials");
        resetWiFiCredentials();
      }
    }
  }
  
 
  lastButtonState = buttonState;
}


void saveBellSchedulesToEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  
 
  EEPROM.write(BELL_ENABLED_ADDR, bellEnabled ? 1 : 0);
  
  
  for (int i = 0; i < MAX_BELLS; i++) {
    int addr = BELL_SCHEDULE_ADDR + (i * 3); 
    EEPROM.write(addr, bellSchedules[i].hour);
    EEPROM.write(addr + 1, bellSchedules[i].minute);
    EEPROM.write(addr + 2, 0);  
  }
  
  EEPROM.commit();
  Serial.println("Bell schedules saved to EEPROM");
}


void loadBellSchedulesFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  

  bellEnabled = EEPROM.read(BELL_ENABLED_ADDR) == 1;
  
  
  for (int i = 0; i < MAX_BELLS; i++) {
    int addr = BELL_SCHEDULE_ADDR + (i * 3);
    bellSchedules[i].hour = EEPROM.read(addr);
    bellSchedules[i].minute = EEPROM.read(addr + 1);
    bellSchedules[i].state = EEPROM.read(addr + 2);
  }
  
  Serial.println("Bell schedules loaded from EEPROM");
  

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


void updateBellStateInEEPROM(int bellIndex, uint8_t state) {
  if (bellIndex < 0 || bellIndex >= MAX_BELLS) return;
  
  EEPROM.begin(EEPROM_SIZE);
  int addr = BELL_SCHEDULE_ADDR + (bellIndex * 3) + 2;  
  EEPROM.write(addr, state);
  EEPROM.commit();
  

  bellSchedules[bellIndex].state = state;
}


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

    
    saveWiFiCredentials(ssid, pass);
    
   
    WiFi.begin(ssid.c_str(), pass.c_str());

    
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


void ringBell(int bellNumber) {
  Serial.println("Bell ringing started for bell #" + String(bellNumber));
  
  if (bellNumber == 1 || bellNumber == 6) {
  
    digitalWrite(BELL_PIN, RELAY_ON);  
    delay(5000);
    digitalWrite(BELL_PIN, RELAY_OFF); 
    delay(1000);
    
    for (int j = 0; j < bellNumber; j++) {
      digitalWrite(BELL_PIN, RELAY_ON);  
      delay(1000);
      digitalWrite(BELL_PIN, RELAY_OFF); 
      delay(1000);
    }
  }
  else if (bellNumber == 5 || bellNumber == 9) {
    // Just a long ring
    digitalWrite(BELL_PIN, RELAY_ON);  
    delay(5000);
    digitalWrite(BELL_PIN, RELAY_OFF); 
  }
  else {
    
    for (int j = 0; j < bellNumber; j++) {
      digitalWrite(BELL_PIN, RELAY_ON);  
      delay(1000);
      digitalWrite(BELL_PIN, RELAY_OFF); 
      delay(1000);
    }
  }
}


void checkAndRingBellsOnline() {
  if (Firebase.ready() && signupOK) {
   
    if (Firebase.RTDB.getInt(&fbdo, "/SchoolBell/status")) {
      int status = fbdo.intData();
      bellEnabled = (status == 1);
      
      if (status == 1) {
       
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
          int bellIndex = i - 1;  
          String pathHour = "/SchoolBell/" + String(i) + "/h";
          String pathMinute = "/SchoolBell/" + String(i) + "/m";
          String pathBellState = "/SchoolBell/" + String(i) + "/state";

          String bellHour, bellMinute, bellState;

       
          if (Firebase.RTDB.getString(&fbdo, pathHour)) {
            bellHour = fbdo.stringData();
            bellSchedules[bellIndex].hour = bellHour.toInt();
            schedulesUpdated = true;
          } else {
            Serial.println("Failed to get Bell Hour: " + fbdo.errorReason());
            continue; 
          }

         
          if (Firebase.RTDB.getString(&fbdo, pathMinute)) {
            bellMinute = fbdo.stringData();
            bellSchedules[bellIndex].minute = bellMinute.toInt();
            schedulesUpdated = true;
          } else {
            Serial.println("Failed to get Bell Minute: " + fbdo.errorReason());
            continue; 
          }

          
          if (Firebase.RTDB.getString(&fbdo, pathBellState)) {
            bellState = fbdo.stringData();
            bellSchedules[bellIndex].state = bellState.toInt();
          } else {
            Serial.println("Failed to get Bell State: " + fbdo.errorReason());
            continue; 
          }

        
          int bellHourInt = bellSchedules[bellIndex].hour;
          int bellMinuteInt = bellSchedules[bellIndex].minute;
          int bellStateInt = bellSchedules[bellIndex].state;

          
          if ((currentHour == bellHourInt) && (currentMinute == bellMinuteInt)) {
            if (bellStateInt == 0) {
              ringBell(i);

            
              if (Firebase.RTDB.setInt(&fbdo, "/SchoolBell/" + String(i) + "/state", 1)) {
                Serial.println("Status updated to 1 successfully!");
              } else {
                Serial.println("Failed to update status to 1");
              }
              
             
              bellSchedules[bellIndex].state = 1;
              updateBellStateInEEPROM(bellIndex, 1);
              
              delay(60000); 

              
              if (Firebase.RTDB.setInt(&fbdo, "/SchoolBell/" + String(i) + "/state", 0)) {
                Serial.println("Status updated to 0 successfully!");
              } else {
                Serial.println("Failed to update status to 0");
              }
              
              
              bellSchedules[bellIndex].state = 0;
              updateBellStateInEEPROM(bellIndex, 0);

              bellRinging = true;
              break; 
            } else {
              Serial.println("Already done.");
            }
          } else {
            Serial.println("No Bell Scheduled for " + String(i));
          }
        }

       
        if (schedulesUpdated) {
          saveBellSchedulesToEEPROM();
        }

        if (!bellRinging) {
          digitalWrite(BELL_PIN, RELAY_OFF); 
        }
      } else {
        Serial.println("Bell status is off");
        digitalWrite(BELL_PIN, RELAY_OFF); 
      }
    } else {
      Serial.println("Failed to get Bell status from Firebase");
      digitalWrite(BELL_PIN, RELAY_OFF); 
    }
  } else {
    Serial.println("Firebase is not ready");
    digitalWrite(BELL_PIN, RELAY_OFF); 
  }
}


void checkAndRingBellsOffline() {
  
  DateTime now = rtc.now();
  int currentHour = now.hour();
  int currentMinute = now.minute();
  
  Serial.print("Offline Mode - Current Time: ");
  Serial.print(currentHour);
  Serial.print(":");
  Serial.println(currentMinute);
  
 
  if (bellEnabled) {
    bool bellRinging = false;
    
    for (int i = 0; i < MAX_BELLS; i++) {
     
      if ((currentHour == bellSchedules[i].hour) && (currentMinute == bellSchedules[i].minute)) {
        if (bellSchedules[i].state == 0) {
          Serial.print("Offline Mode - Ringing bell #");
          Serial.println(i + 1);
          
          ringBell(i + 1);
          
          // Update local state
          bellSchedules[i].state = 1;
          updateBellStateInEEPROM(i, 1);
          
          delay(60000);
          
     
          bellSchedules[i].state = 0;
          updateBellStateInEEPROM(i, 0);
          
          bellRinging = true;
          break;
        } else {
          Serial.println("Offline Mode - Bell already rung.");
        }
      }
    }
    
    if (!bellRinging) {
      digitalWrite(BELL_PIN, RELAY_OFF); 
    }
  } else {
    Serial.println("Offline Mode - Bell system is disabled");
    digitalWrite(BELL_PIN, RELAY_OFF); 
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  
  
  pinMode(BELL_PIN, OUTPUT);
  digitalWrite(BELL_PIN, RELAY_OFF); 
  
 
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  

  Wire.begin(SDA_PIN, SCL_PIN);
  
  
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC! Check wiring.");
    while (1);
  }
  

  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting to compile time!");
   
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/networks", handleNetworks);

  
  digitalWrite(BELL_PIN, RELAY_OFF);
  
  
  loadBellSchedulesFromEEPROM();
  
  if (!connectToWiFiFromEEPROM()) {
    startAPMode();
  } else {
    Serial.println("Connected to Wi-Fi");
    
    // Configure Firebase
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    

    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase sign-up successful");
      signupOK = true;
    } else {
      Serial.printf("Firebase sign-up failed: %s\n", config.signer.signupError.message.c_str());
    }
    
  
    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    
   
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
    
    
    server.begin();
    
    
    digitalWrite(BELL_PIN, RELAY_OFF);
  }
}

void loop() {
  server.handleClient();
  dnsServer.processNextRequest();
  
  
  checkResetButton();
  

  if (WiFi.status() == WL_CONNECTED) {
  
    checkAndRingBellsOnline();
  } else {

    Serial.println("WiFi not connected - using saved bell schedules from EEPROM");
    checkAndRingBellsOffline();
    digitalWrite(BELL_PIN, RELAY_OFF); 
  }
  
  delay(100);
}
