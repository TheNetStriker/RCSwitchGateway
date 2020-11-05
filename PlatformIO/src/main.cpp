#include "Arduino.h"
#include "RCSwitch.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <QueueList.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#define ESP_DRD_USE_EEPROM false
#define ESP_DRD_USE_SPIFFS true

#include <ESP_DoubleResetDetector.h>

// Number of seconds after reset during which a 
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

DoubleResetDetector* drd;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
RCSwitch mySwitch = RCSwitch();

WiFiManager wifiManager;
Ticker ticker;

char hostname[40] = "rcswitch01";
char hostnameLowerCase[40];
char mqtt_server[40];
char mqtt_port[6] = "1883";

const char* HOSTNAME_ID = "hostname";
const char* MQTT_SERVER_ID = "mqtt_server";
const char* MQTT_PORT_ID = "mqtt_port";

WiFiManagerParameter custom_hostname(HOSTNAME_ID, "Hostname", hostname, 40);
WiFiManagerParameter custom_mqtt_server(MQTT_SERVER_ID, "MQTT server", mqtt_server, 40);
WiFiManagerParameter custom_mqtt_port(MQTT_PORT_ID, "MQTT port", mqtt_port, 6);

int receivePin = 15;
int transmitPin = 32;

bool otaUpdateRunning = false;
uint8_t otaProgress = 0;

// MQTT Topics
String deviceTopic;
String willTopic;

String systemNodeTopic;
String senderNodeTopic;
String receiverNodeTopic;

String rssiPropertyTopic;
String logPropertyTopic;
String sendTypeAPropertyTopic;
String sendTypeASetPropertyTopic;
String sendPropertyTopic;
String sendSetPropertyTopic;
String queueLengthPropertyTopic;
String codeReceivedPropertyTopic;

class CodeQueueItem
{
  public:
    unsigned long code;
    unsigned int length;
    int protocol;
    int repeatTransmit;
    virtual void f() { }
};

const unsigned int maxQueueCount = 30;
QueueList <CodeQueueItem> queue;

unsigned long rssiTimer = 0;
const unsigned long rssiTimeout = 60000;

void toLower(char* output, const char* input) {
  strcpy(output, input);

  for (char *iter = output; *iter != '\0'; ++iter) {
    *iter = std::tolower(*iter);
  }
}

void blink()
{
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

void sendRSSI() {
  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.print(F("WiFi RSSI: "));
    Serial.println(WiFi.RSSI());
  #endif
  mqttClient.publish(rssiPropertyTopic.c_str(), String(WiFi.RSSI()).c_str(), true);
}

bool checkAndConnectMqtt() {
  if (!mqttClient.connected()) {
    digitalWrite(LED_BUILTIN, HIGH);
    ticker.attach(1, blink);
        
    if (strlen(mqtt_server) != 0) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("MQTT connecting..."));
      #endif

      mqttClient.setServer(mqtt_server, String(mqtt_port).toInt());
      mqttClient.setKeepAlive(60);

      while (!mqttClient.connect(hostname, willTopic.c_str(), 0, true, "lost")) {
        int errorCode = mqttClient.state();
        #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
          Serial.print(F("MQTT connect error: "));
          Serial.println(errorCode);
        #endif

        ticker.detach();
        return false;
      }

      mqttClient.subscribe(sendTypeASetPropertyTopic.c_str());
      mqttClient.subscribe(sendSetPropertyTopic.c_str());

      mqttClient.publish(willTopic.c_str(), "ready", true);

      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("MQTT connected!"));
      #endif

      sendRSSI();

      ticker.detach();
      digitalWrite(LED_BUILTIN, LOW);
    }
  }

  return true;
}

bool autoConnectWifi() {
  digitalWrite(LED_BUILTIN, LOW);
  ticker.attach(0.5, blink);

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // Workarround for hostname problem https://github.com/espressif/arduino-esp32/issues/806
  wifiManager.setHostname(hostname);
  bool success = wifiManager.autoConnect(hostname);

  if (success) {
    // Disable default access point
    WiFi.mode(WIFI_STA);

    if (MDNS.begin(hostname)) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.print(F("MDNS responder started: "));
        Serial.println(hostname);
      #endif
    }
  }

  ticker.detach();
  digitalWrite(LED_BUILTIN, HIGH);

  return success;
}

void checkAndConnectWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, LOW);
    ticker.attach(0.5, blink);
    
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // Workarround for hostname problem https://github.com/espressif/arduino-esp32/issues/806
    WiFi.mode(WIFI_STA); // Disable default access point
    WiFi.setHostname(hostname);
    WiFi.begin();

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("Connecting to "));
      Serial.print(wifiManager.getWiFiSSID());
      Serial.println(F(" ..."));
    #endif

    int i = 0;
    while (WiFi.status() != WL_CONNECTED) { // Wait for the Wi-Fi to connect
      delay(1000);
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.print(++i);
        Serial.print(F(" "));
      #endif

      if (WiFi.status() == WL_CONNECT_FAILED) {
        #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
          Serial.println();
          Serial.println(F("Connect failed"));
        #endif
        delay(5000);
        WiFi.begin();
        #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
          Serial.print(F("."));
        #endif
      }
    }

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.println(F("\n"));
      Serial.println(F("Connection established!"));  
      Serial.print(F("IP address:\t"));
      Serial.println(WiFi.localIP());
    #endif

    if (MDNS.begin(hostname)) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.print(F("MDNS responder started: "));
        Serial.println(hostname);
      #endif
    }

    ticker.detach();
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

void messageReceived(char* topic, const byte* payload, unsigned int length) {
  String topicString = topic;

  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.print(F("incoming message: "));
    Serial.println(topicString);
  #endif

  if (queue.count() > maxQueueCount) {
    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.println(F("Error: Send queue is full!"));
    #endif
    return;
  }

  StaticJsonDocument<255> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  JsonObject json = doc.as<JsonObject>();

  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.print(F("json: "));
    serializeJson(doc, Serial);
    Serial.println();
  #endif
  
  if (error) {
    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.c_str());
    #endif
    return;
  }

  if (topicString == sendTypeASetPropertyTopic) {
    //example request: {"group": "11111", "device": "11111", "repeatTransmit": 5, "switchOnOff": true}

    if (!json.containsKey("group") || !json.containsKey("device") || !json.containsKey("repeatTransmit") || !json.containsKey("repeatTransmit")) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("Values missing!"));
      #endif
      return;
    }
    
    String group = json["group"];
    String device = json["device"];
    int repeatTransmit = json["repeatTransmit"];
    bool switchOnOff = json["switchOnOff"];

    char* sCodeWord = mySwitch.getCodeWordA(group.c_str(), device.c_str(), switchOnOff);
    unsigned long code = 0;
    unsigned int length = 0;
    mySwitch.triStateGetCodeAndLength(sCodeWord, code, length);

    CodeQueueItem *item = new CodeQueueItem();

    item->code = code;
    item->length = length;
    item->protocol = 1;
    item->repeatTransmit = repeatTransmit;

    queue.push(*item);

    // Send message to main topic to confirm message received
    mqttClient.publish(sendTypeAPropertyTopic.c_str(), payload, length, true);

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("sendtypea added to queue, group: "));
      Serial.print(group);
      Serial.print(F(" device: "));
      Serial.print(device);
      Serial.print(F(" repeatTransmit: "));
      Serial.print(repeatTransmit);
      Serial.print(F(" switch: "));
      Serial.println(switchOnOff);
    #endif
  } else if (topicString == sendSetPropertyTopic) {
    //example request: {"code": 1234, "codeLength": 24, "protocol": 1, "repeatTransmit": 5 }

    if (!json.containsKey("code") || !json.containsKey("codeLength") || !json.containsKey("protocol") || !json.containsKey("repeatTransmit")) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("Values missing!"));
      #endif
      return;
    }
    
    int code = json["code"];
    int codeLength = json["codeLength"];
    int protocol = json["protocol"];
    int repeatTransmit = json["repeatTransmit"];

    CodeQueueItem *item = new CodeQueueItem();

    item->code = code;
    item->length = codeLength;
    item->protocol = protocol;
    item->repeatTransmit = repeatTransmit;

    queue.push(*item);

    // Send message to main topic to confirm message received
    mqttClient.publish(sendPropertyTopic.c_str(), payload, length, true);

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("send added to queue, code: "));
      Serial.print(code);
      Serial.print(F(" codeLength: "));
      Serial.print(codeLength);
      Serial.print(F(" protocol: "));
      Serial.print(protocol);
      Serial.print(F(" repeatTransmit: "));
      Serial.println(repeatTransmit);
    #endif
  }
}

void saveParamsCallback () {
  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.println(F("saveParamsCallback"));
  #endif

  strcpy(hostname, custom_hostname.getValue());
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());

  //save the custom parameters to FS
  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.println(F("saving config"));
  #endif

  DynamicJsonDocument doc(1024);

  doc[HOSTNAME_ID] = hostname;
  doc[MQTT_SERVER_ID] = mqtt_server;
  doc[MQTT_PORT_ID] = mqtt_port;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.println(F("failed to open config file for writing"));
    #endif
    return;
  }

  serializeJson(doc, Serial);
  serializeJson(doc, configFile);

  configFile.close();
}

void readConfig() {
  if (SPIFFS.exists("/config.json")) {
    //file exists, reading and loading
    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.println(F("reading config file"));
    #endif
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("opened config file"));
      #endif
      size_t size = configFile.size();
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);

      configFile.readBytes(buf.get(), size);
      DynamicJsonDocument doc(size);
      auto error = deserializeJson(doc, buf.get());

      if (error) {
        #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
          Serial.print(F("deserializeJson() failed with code "));
          Serial.println(error.c_str());
        #endif
        return;
      }
      
      serializeJson(doc, Serial);

      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("\nparsed json"));
      #endif

      if (doc.containsKey(HOSTNAME_ID) && doc[HOSTNAME_ID] != "") {
        strcpy(hostname, doc[HOSTNAME_ID]);
        custom_hostname.setValue(hostname, 40);
      }

      if (doc.containsKey(MQTT_SERVER_ID) && doc[MQTT_SERVER_ID] != "") {
        strcpy(mqtt_server, doc[MQTT_SERVER_ID]);
        custom_mqtt_server.setValue(mqtt_server, 40);
      }

      if (doc.containsKey(MQTT_PORT_ID) && doc[MQTT_PORT_ID] != "") {
        strcpy(mqtt_port, doc[MQTT_PORT_ID]);
        custom_mqtt_port.setValue(mqtt_port, 6);
      }

      configFile.close();

      toLower(hostnameLowerCase, hostname);

      deviceTopic = String("homie/") + hostnameLowerCase;
      willTopic = deviceTopic + String("/$state");

      systemNodeTopic = deviceTopic + "/system";
      senderNodeTopic = deviceTopic + "/sender";
      receiverNodeTopic = deviceTopic + "/receiver";

      rssiPropertyTopic = systemNodeTopic + "/rssi";
      logPropertyTopic = systemNodeTopic + "/log";

      sendTypeAPropertyTopic = senderNodeTopic + "/sendtypea";
      sendTypeASetPropertyTopic = sendTypeAPropertyTopic + "/set";
      sendPropertyTopic = senderNodeTopic + "/send";
      sendSetPropertyTopic = sendPropertyTopic + "/set";

      queueLengthPropertyTopic = receiverNodeTopic + "/queuelength";
      codeReceivedPropertyTopic = receiverNodeTopic + "/codereceived";
    }
  }
}

bool initSPIFFS() {
  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.println(F("mounting FS..."));
  #endif

  if (!SPIFFS.begin()) {
    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.println(F("Formatting FS..."));
    #endif
    SPIFFS.format();

    if (!SPIFFS.begin()) {
      return false;
    }
  }

  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.println(F("mounted file system"));
  #endif
  return true;
}

void setupOTA() {
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false);
  Serial.println(OTA_PASSWORD);

  ArduinoOTA
    .onStart([]() {
      otaUpdateRunning = true;
      otaProgress = 0;

      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.print(F("Start updating "));
        Serial.println(type);
      #endif
    })
    .onEnd([]() {
      otaUpdateRunning = false;
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("\nEnd"));
      #endif
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      uint8_t currentProgress = (progress / (total / 100));
        if (currentProgress > otaProgress) {
          #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
            Serial.print(F("Progress: "));
            Serial.println(currentProgress);
          #endif
          otaProgress = currentProgress;
        }
    })
    .onError([](ota_error_t error) {
      otaUpdateRunning = false;
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.print(F("Error: "));
        Serial.println(error);
        if (error == OTA_AUTH_ERROR) Serial.println(F(" Auth Failed"));
        else if (error == OTA_BEGIN_ERROR) Serial.println(F(" Begin Failed"));
        else if (error == OTA_CONNECT_ERROR) Serial.println(F(" Connect Failed"));
        else if (error == OTA_RECEIVE_ERROR) Serial.println(F(" Receive Failed"));
        else if (error == OTA_END_ERROR) Serial.println(F(" End Failed"));
      #endif
    });

  ArduinoOTA.begin();
}

#if defined(HOMIE_DISCOVERY) && HOMIE_DISCOVERY
void sendHomieDiscovery() {
  mqttClient.publish((rssiPropertyTopic + "/$name").c_str(), "Wifi RSSI", true);
  mqttClient.publish((rssiPropertyTopic + "/$unit").c_str(), "dB", true);
  mqttClient.publish((rssiPropertyTopic + "/$datatype").c_str(), "integer", true);
  mqttClient.publish((rssiPropertyTopic + "/$format").c_str(), "-100:0", true);

  mqttClient.publish((logPropertyTopic + "/$name").c_str(), "Debug log", true);
  mqttClient.publish((logPropertyTopic + "/$datatype").c_str(), "string", true);
  mqttClient.publish((logPropertyTopic + "/$retained").c_str(), "false", true);

  mqttClient.publish((systemNodeTopic + "/$name").c_str(), "System", true);
  mqttClient.publish((systemNodeTopic + "/$properties").c_str(), "rssi,log", true);

  mqttClient.publish((sendTypeAPropertyTopic + "/$name").c_str(), "Send type a signal", true);
  mqttClient.publish((sendTypeAPropertyTopic + "/$datatype").c_str(), "string", true);
  mqttClient.publish((sendTypeAPropertyTopic + "/$settable").c_str(), "true", true);

  mqttClient.publish((sendPropertyTopic + "/$name").c_str(), "Send signal", true);
  mqttClient.publish((sendPropertyTopic + "/$datatype").c_str(), "string", true);
  mqttClient.publish((sendPropertyTopic + "/$settable").c_str(), "true", true);

  mqttClient.publish((senderNodeTopic + "/$name").c_str(), "Sender", true);
  mqttClient.publish((senderNodeTopic + "/$properties").c_str(), "sendtypea,send", true);

  mqttClient.publish((queueLengthPropertyTopic + "/$name").c_str(), "Sender queue length", true);
  mqttClient.publish((queueLengthPropertyTopic + "/$datatype").c_str(), "integer", true);

  mqttClient.publish((codeReceivedPropertyTopic + "/$name").c_str(), "Code received event", true);
  mqttClient.publish((codeReceivedPropertyTopic + "/$datatype").c_str(), "integer", true);
  mqttClient.publish((codeReceivedPropertyTopic + "/$retained").c_str(), "false", true);

  mqttClient.publish((receiverNodeTopic + "/$name").c_str(), "Receiver", true);
  mqttClient.publish((receiverNodeTopic + "/$properties").c_str(), "queuelength,codereceived", true);

  mqttClient.publish((deviceTopic + "/$homie").c_str(), "4.0", true);
  mqttClient.publish((deviceTopic + "/$name").c_str(), hostname, true);
  mqttClient.publish((deviceTopic + "/$nodes").c_str(), "system,sender,receiver", true);
  mqttClient.publish((deviceTopic + "/$implementation").c_str(), "ESP32", true);
  mqttClient.publish((deviceTopic + "/$extensions").c_str(), "", true);
  mqttClient.publish(willTopic.c_str(), "ready", true);
}
#endif

void setup() {
  #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
    Serial.begin(115200);
  #else
    wifiManager.setDebugOutput(false);
  #endif

  delay(1000);

  pinMode(LED_BUILTIN, OUTPUT);

  mqttClient.setCallback(messageReceived);

  if (initSPIFFS()) {
    readConfig();

    drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);

    wifiManager.addParameter(&custom_hostname);
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);

    wifiManager.setSaveParamsCallback(saveParamsCallback);

    if (drd->detectDoubleReset()) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("Double Reset Detected"));
      #endif

      digitalWrite(LED_BUILTIN, HIGH);
      ticker.attach(0.5, blink);

      wifiManager.startConfigPortal(hostname);

      ticker.detach();
      digitalWrite(LED_BUILTIN, LOW);
      ESP.restart();
    } else {
      mySwitch.enableReceive(receivePin);
      mySwitch.enableTransmit(transmitPin);
      mySwitch.setProtocol(2);
      mySwitch.setRepeatTransmit(5);

      if (autoConnectWifi()) {
        readConfig(); // Read config again in case something changed in the portal.
        checkAndConnectMqtt();
        setupOTA();

        #if defined(HOMIE_DISCOVERY) && HOMIE_DISCOVERY
          sendHomieDiscovery();
        #endif

        mqttClient.publish(logPropertyTopic.c_str(), "Startup");
      } else {
        ESP.restart();
      }
    }
  }
}

void loop() {
  if (!otaUpdateRunning) checkAndConnectWifi();
  if (!otaUpdateRunning) checkAndConnectMqtt();

  if (!otaUpdateRunning && mySwitch.available()) {
    unsigned long value = mySwitch.getReceivedValue();
    if (value == 0) {
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.println(F("Unknown encoding"));
      #endif
    } else {
      mqttClient.publish(codeReceivedPropertyTopic.c_str(), String(value).c_str());
      #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
        Serial.print(F("code received "));
        Serial.println(value);
      #endif
    }
    mySwitch.resetAvailable();
  }

  if (!otaUpdateRunning && !queue.isEmpty()) {
    CodeQueueItem item = queue.pop();

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("sending code: "));
      Serial.print(item.code);
      Serial.print(F(" length: "));
      Serial.print(item.length);
      Serial.print(F(" protocol: "));
      Serial.print(item.protocol);
      Serial.print(F(" repeatTransmit: "));
      Serial.println(item.repeatTransmit);
    #endif

    mySwitch.setProtocol(item.protocol);
    mySwitch.setRepeatTransmit(item.repeatTransmit);
    mySwitch.send(item.code, item.length);

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("sent code: "));
      Serial.print(item.code);
      Serial.print(F(" length: "));
      Serial.print(item.length);
      Serial.print(F(" protocol: "));
      Serial.print(item.protocol);
      Serial.print(F(" repeatTransmit: "));
      Serial.println(item.repeatTransmit);
    #endif

    int queueCount = queue.count();
    mqttClient.publish(queueLengthPropertyTopic.c_str(), String(queueCount).c_str(), true);

    #if defined(RC_SWITCH_DEBUG) && RC_SWITCH_DEBUG
      Serial.print(F("Queue count: "));
      Serial.println(queueCount);
    #endif
  }

  if (!otaUpdateRunning && (unsigned long)(millis() - rssiTimer) >= rssiTimeout) {
    // Send RSSI
    rssiTimer = millis();
    sendRSSI();
  }

  if (!otaUpdateRunning) mqttClient.loop();
  if (!otaUpdateRunning) drd->loop();
  ArduinoOTA.handle();
}
