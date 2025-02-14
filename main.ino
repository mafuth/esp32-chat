#include <WiFi.h>
#include <SPIFFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoUniqueID.h>
#include "HT_lCMEN2R13EFC1.h"
#include "mbedtls/base64.h"

// Wi-Fi credentials
String wifi_SSID = "";
String wifi_Password = "";

// MQTT broker details
const char* mqttServer = ""; // MQTT Server Host
const int mqttPort = 0;
String mqttReciveTopic = ""; // Topic to subscribe messages

// Configuration
const char* configPath = "/config.txt";
const char* version = "0.1.4";
int screenWidth, screenHeight;
String lastMsg = "";
int checkInInterval = 60000; // checkin every 1 mins
int checkInStatus = 0;

// Startup
WiFiClient espClient;
PubSubClient mqttClient(espClient);
String uuid;
HT_ICMEN2R13EFC1 display(6, 5, 4, 7, 3, 2, -1, 6000000);

// Function to read a file
String readFile(const char* path) {
  File file = SPIFFS.open(path, "r");
  if (!file) {
    return "";
  }
  String content = file.readString();
  file.close();
  return content;
}

// Function to Base64 encode a string
String base64Encode(const String &data) {
  size_t inputLength = data.length();
  size_t encodedLength = 4 * ((inputLength + 2) / 3); // Calculate encoded size
  unsigned char encoded[encodedLength + 1];           // +1 for null terminator
  size_t outputLength;

  if (mbedtls_base64_encode(encoded, sizeof(encoded), &outputLength, (const unsigned char *)data.c_str(), inputLength) == 0) {
    encoded[outputLength] = '\0'; // Null-terminate the encoded string
    return String((char *)encoded);
  } else {
    return String(""); // Return empty string if encoding fails
  }
}

// Function to Base64 decode a string
String base64Decode(const String &data) {
  size_t inputLength = data.length();
  size_t decodedLength = (inputLength * 3) / 4; // Approximate decoded size
  unsigned char decoded[decodedLength + 1];    // +1 for null terminator
  size_t outputLength;

  if (mbedtls_base64_decode(decoded, sizeof(decoded), &outputLength, (const unsigned char *)data.c_str(), inputLength) == 0) {
    decoded[outputLength] = '\0'; // Null-terminate the decoded string
    return String((char *)decoded);
  } else {
    return String(""); // Return empty string if decoding fails
  }
}

void writeFile(const char* path, String content) {
  File file = SPIFFS.open(path, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.print(content);
  file.close();
}

void setup() {
  Serial.begin(115200);
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount config");
    return;
  }

  screenWidth = display._width;
  screenHeight = display._height;
  VextON();
  delay(100);
  display.init();

  // Generate UUID for this board
  uuid = "";
  for (size_t i = 0; i < UniqueIDsize; i++) {
    if (UniqueID[i] < 0x10) uuid += "0";
    uuid += String(UniqueID[i], HEX);
  }

  //loading config
  StaticJsonDocument<500> config;
  String savedConfigJson = readFile(configPath);
  if(savedConfigJson.length() > 0){
    DeserializationError error = deserializeJson(config, savedConfigJson);
    if (error) {
      Serial.println("Failed to parse configuration");
      return;
    }else{
      wifi_SSID = config["wifi_SSID"].as<String>();
      wifi_Password = config["wifi_Password"].as<String>();
      mqttReciveTopic = config["mqttReciveTopic"].as<String>();
      lastMsg = config["lastMsg"].as<String>();
      if(lastMsg == ""){
        lastMsg = "No saved message available";
      }
      connectToWiFi();
      connectToMQTT();
      delay(5000);
      displayMessage(lastMsg);
    }
  }else{
    Serial.println("No configuration found");
    displayMessage("Visit https://mafuth.github.io/esp32-chat/ to configure!");
  }
}

void loop() {
  if (Serial.available()) {
      String jsonString = Serial.readStringUntil('\n');
      jsonString.trim();

      // Parse the incoming JSON data
      StaticJsonDocument<200> incomingJson;
      DeserializationError error = deserializeJson(incomingJson, jsonString);

      if (error) {
        Serial.println("Failed to parse JSON");
        return;
      }

      if (incomingJson["command"] == "GET_CONFIG") {
        sendJsonConfig();
      }
      else if (incomingJson["command"] == "SCAN_NETWORKS") {
        delay(5000);
        // Scan for Wi-Fi networks
        int numNetworks = WiFi.scanNetworks();
        StaticJsonDocument<500> outgoingJson;
        outgoingJson["command"] = "SCAN_RESULT";
        JsonArray networks = outgoingJson.createNestedArray("networks");
        for (int i = 0; i < numNetworks; i++) {
          networks.add(WiFi.SSID(i));
        }
        serializeJson(outgoingJson, Serial);
        Serial.println(); // Add a newline for readability
      }
      else if (incomingJson["command"] == "SAVE_CONFIG") {
        // Extract SSID and password
        wifi_SSID = incomingJson["ssid"].as<String>();
        wifi_Password = incomingJson["password"].as<String>();
        mqttReciveTopic = incomingJson["exhchange"].as<String>();

        // Connect to Wi-Fi & MQTT
        connectToWiFi();
        connectToMQTT();
      }
      else if (incomingJson["command"] == "RESET_CONFIG") {
        // Reset the configuration
        SPIFFS.remove(configPath);
        sendJsonCommand("STATUS_UPDATE", "Configuration reseted. Please reconnect.");
      }
      else if (incomingJson["command"] == "SEND_MQTT") {
        // Publish JSON message to MQTT
        // Maintain MQTT connection
        if (!mqttClient.connected()) {
          checkInStatus = 0;
          connectToMQTT();
        }
        if (mqttClient.connected()) {
          sendMqttMessage(incomingJson["message"].as<String>(), "message");
          sendJsonCommand("STATUS", "Message published to MQTT");
        }else{
          sendJsonCommand("STATUS", "Message failed to publish on MQTT");
        }
      }
  }
  // Maintain MQTT connection
  if (!mqttClient.connected()) {
    checkInStatus = 0;
    connectToMQTT();
  }
  mqttClient.loop();

  static unsigned long lastCheckIn = 0;
  checkInStatus = 0;
  if (millis() - lastCheckIn > checkInInterval) {
    lastCheckIn = millis();
    sendMqttMessage(mqttReciveTopic.c_str(), "keepAlive");
  }
}

void sendJsonConfig() {
  StaticJsonDocument<500> outgoingJson;
  outgoingJson["command"] = "CONFIG_RESULT";
  outgoingJson["version"] = version;
  outgoingJson["uuid"] = uuid;
  outgoingJson["mqttSendTopic"] = uuid;
  outgoingJson["mqttReciveTopic"] = mqttReciveTopic;
  outgoingJson["mqttServer"] = mqttServer;
  outgoingJson["mqttPort"] = mqttPort;
  outgoingJson["checkInStatus"] = checkInStatus;
  if(WiFi.status() != WL_CONNECTED){
    outgoingJson["mqtt_Status"] = "Not Connected";
    outgoingJson["wifi_SSID"] = wifi_SSID;
    outgoingJson["wifi_Password"] = wifi_Password;
    outgoingJson["wifi_Status"] = "Not Connected";
    outgoingJson["wifi_IP"] = "";
  }else{
    if(!mqttClient.connected()){
      outgoingJson["mqtt_Status"] = "Not Connected";
      outgoingJson["wifi_SSID"] = wifi_SSID;
      outgoingJson["wifi_Password"] = wifi_Password;
      outgoingJson["wifi_Status"] = "Connected";
      outgoingJson["wifi_IP"] = WiFi.localIP().toString();
    }else{
      outgoingJson["mqtt_Status"] = "Connected";
      outgoingJson["wifi_SSID"] = wifi_SSID;
      outgoingJson["wifi_Password"] = wifi_Password;
      outgoingJson["wifi_Status"] = "Connected";
      outgoingJson["wifi_IP"] = WiFi.localIP().toString();
    }

  }
  serializeJson(outgoingJson, Serial);
  Serial.println();
}

void sendJsonCommand(String command, String message) {
  StaticJsonDocument<200> outgoingJson;
  outgoingJson["command"] = command;
  outgoingJson["message"] = message;
  serializeJson(outgoingJson, Serial);
  Serial.println();
}

void displayMessage(String message) {
    // create more fonts at http://oleddisplay.squix.ch/
    
    display.clear();

    display.setFont(ArialMT_Plain_24);
    display.drawStringMaxWidth(0, 0, screenWidth, message);

    display.update(BLACK_BUFFER);
    display.display();
}

void connectToWiFi() {
  if(wifi_SSID != "" && wifi_Password != ""){
    WiFi.begin(wifi_SSID.c_str(), wifi_Password.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }

    // Send connection status and IP address
    if (WiFi.status() == WL_CONNECTED) {
      mqttClient.setServer(mqttServer, mqttPort);
      mqttClient.setCallback(mqttCallback);
      sendJsonCommand("STATUS_UPDATE", "WIFI Connected");
      connectToMQTT();
      saveConfig();
      sendJsonConfig();
    } else {
      sendJsonCommand("STATUS_UPDATE", "WIFI failed to connect");
    }
  }
}

void saveConfig() {
  StaticJsonDocument<500> config;
  config["wifi_SSID"] = wifi_SSID;
  config["wifi_Password"] = wifi_Password;
  config["mqttReciveTopic"] = mqttReciveTopic;
  config["lastMsg"] = lastMsg;
  String output;
  serializeJson(config, output);
  writeFile(configPath, output);
}

void connectToMQTT() {
  if(wifi_SSID != "" && wifi_Password != ""){
    while (!mqttClient.connected()){
      if (mqttClient.connect(uuid.c_str())) {
        mqttClient.subscribe(mqttReciveTopic.c_str()); // Subscribe to the topic
        sendJsonCommand("STATUS_UPDATE", "MQTT Connected");
        saveConfig();
        sendJsonConfig();
        sendMqttMessage(mqttReciveTopic.c_str(), "keepAlive");
      } else {
        delay(5000);
      }
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = String((char*)payload, length);

  // Parse the incoming JSON message
  StaticJsonDocument<200> incomingJson;
  DeserializationError error = deserializeJson(incomingJson, base64Decode(message));

  if (error) {
    Serial.println("Failed to parse JSON: " + message);
    return;
  }

  // Check the message type
  if (incomingJson["type"] == "message") {
    lastMsg = incomingJson["message"].as<String>();
    sendJsonCommand("MQTT_MESSAGE_RECIVED", lastMsg);
    saveConfig();
    delay(1000);                // Wait for 1 second
    displayMessage(incomingJson["message"].as<String>());
  }
  else if (incomingJson["type"] == "keepAlive") {
    if(incomingJson["message"].as<String>() == uuid){
      sendMqttMessage(uuid, "alive");
    }
  }
  else if (incomingJson["type"] == "alive") {
    if(incomingJson["message"].as<String>() == mqttReciveTopic){
      checkInStatus = 1;
    }
  }
}

void sendMqttMessage(String message, String type) {
  StaticJsonDocument<200> outgoingJson;
  outgoingJson["type"] = type;
  outgoingJson["message"] = message;
  String jsonString;
  serializeJson(outgoingJson, jsonString);
  mqttClient.publish(uuid.c_str(), base64Encode(jsonString).c_str());
}

void VextON(void){
  pinMode(45, OUTPUT);
  digitalWrite(45, LOW);
}

void VextOFF(void){
  pinMode(45, OUTPUT);
  digitalWrite(45, HIGH);
}