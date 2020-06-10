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

#define ESP_DRD_USE_EEPROM false
#define ESP_DRD_USE_SPIFFS true
#define DOUBLERESETDETECTOR_DEBUG true

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

String sendTypeATopic;
String sendTopic;
String queueLengthTopic;
String codeEventTopic;

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

const bool debug = true;

void blink()
{
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

bool checkAndConnectMqtt() {
  if (!mqttClient.connected()) {
    digitalWrite(LED_BUILTIN, HIGH);
    ticker.attach(1, blink);
        
    if (strlen(mqtt_server) != 0) {
      Serial.println("MQTT connecting...");

      mqttClient.setServer(mqtt_server, String(mqtt_port).toInt());

      while (!mqttClient.connect(hostname)) {
        int errorCode = mqttClient.state();
        Serial.println("MQTT connect error: " + String(errorCode) + " ");

        ticker.detach();
        return false;
      }

      mqttClient.subscribe(sendTypeATopic.c_str());
      mqttClient.subscribe(sendTopic.c_str());

      Serial.println("MQTT connected!");

      ticker.detach();
      digitalWrite(LED_BUILTIN, LOW);
    }
  }

  return true;
}

bool autoConnectWifi() {
  digitalWrite(LED_BUILTIN, LOW);
  ticker.attach(0.5, blink);

  bool success = wifiManager.autoConnect(hostname);

  if (success) {
      if (MDNS.begin(hostname)) {
        Serial.println("MDNS responder started: " + String(hostname));
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
    
    WiFi.begin();
    Serial.print("Connecting to ");
    Serial.print(wifiManager.getWiFiSSID());
    Serial.println(" ...");

    int i = 0;
    while (WiFi.status() != WL_CONNECTED) { // Wait for the Wi-Fi to connect
      delay(1000);
      Serial.print(++i); Serial.print(' ');

      if (WiFi.status() == WL_CONNECT_FAILED) {
        Serial.println("");
        Serial.println("Connect failed");
        delay(5000);
        WiFi.begin();
        Serial.print(".");
      }
    }

    Serial.println('\n');
    Serial.println("Connection established!");  
    Serial.print("IP address:\t");
    Serial.println(WiFi.localIP());

    if (MDNS.begin(hostname)) {
      Serial.println("MDNS responder started: " + String(hostname));
    }

    ticker.detach();
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

void messageReceived(char* topic, byte* payload, unsigned int length) {
  String topicString = topic;

  Serial.println("incoming message: " + topicString);

  if (queue.count() > maxQueueCount) {
    if (debug)
      Serial.println("Error: Send queue is full!");
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload, length);
  JsonObject json = doc.as<JsonObject>();

  Serial.print("json: ");
  serializeJson(doc, Serial);
  Serial.println("");
  
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    return;
  }

  if (topicString == sendTypeATopic) {
    //example request: {"group": "11111", "device": "11111", "repeatTransmit": 5, "switchOnOff": true}

    if (!json.containsKey("group") || !json.containsKey("device") || !json.containsKey("repeatTransmit") || !json.containsKey("repeatTransmit")) {
      Serial.println("Values missing!");
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

    if (debug)
      Serial.println("sendtypea added to queue, group: " + group + " device: " + device + " repeatTransmit: " + String(repeatTransmit) + " switch: " + switchOnOff);
  } else if (topicString == sendTopic) {
    //example request: {"code": 1234, "codeLength": 24, "protocol": 1, "repeatTransmit": 5 }

    if (!json.containsKey("code") || !json.containsKey("codeLength") || !json.containsKey("protocol") || !json.containsKey("repeatTransmit")) {
      Serial.println("Values missing!");
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

    if (debug)
      Serial.println("send added to queue, code: " + String(code) + " codeLength: " + String(codeLength) + " protocol: " + String(protocol) + " repeatTransmit: " + String(repeatTransmit));
  }
}

void saveParamsCallback () {
  Serial.println("saveParamsCallback");

  strcpy(hostname, custom_hostname.getValue());
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());

  //save the custom parameters to FS
  Serial.println("saving config");

  DynamicJsonDocument doc(1024);

  doc[HOSTNAME_ID] = hostname;
  doc[MQTT_SERVER_ID] = mqtt_server;
  doc[MQTT_PORT_ID] = mqtt_port;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
    return;
  }

  serializeJson(doc, Serial);
  serializeJson(doc, configFile);

  configFile.close();
}

void readConfig() {
  if (SPIFFS.exists("/config.json")) {
    //file exists, reading and loading
    Serial.println("reading config file");
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      Serial.println("opened config file");
      size_t size = configFile.size();
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);

      configFile.readBytes(buf.get(), size);
      DynamicJsonDocument doc(size);
      auto error = deserializeJson(doc, buf.get());

      if (error) {
          Serial.print(F("deserializeJson() failed with code "));
          Serial.println(error.c_str());
          return;
      }
      
      serializeJson(doc, Serial);

      Serial.println("\nparsed json");

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

      sendTypeATopic = String("/") + hostname + String("/sender/sendtypea");
      sendTopic = String("/") + hostname + "/sender/send";
      queueLengthTopic = String("/") + hostname + "/queue/length";
      codeEventTopic = String("/") + hostname + "/events/codereceived";
    }
  }
}

bool initSPIFFS() {
  Serial.println("mounting FS...");

  if (!SPIFFS.begin()) {
    Serial.println("Formatting FS...");
    SPIFFS.format();

    if (!SPIFFS.begin()) {
      return false;
    }
  }

  Serial.println("mounted file system");
  return true;
}

void setup() {
  Serial.begin(115200);

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
      Serial.println("Double Reset Detected");

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

      digitalWrite(LED_BUILTIN, HIGH);
      ticker.attach(0.5, blink);

      if (autoConnectWifi()) {
        ticker.detach();
        digitalWrite(LED_BUILTIN, LOW);

        if (MDNS.begin(hostname)) {
          Serial.println("MDNS responder started");
        }

        readConfig(); // Read config again in case something changed in the portal.
        checkAndConnectMqtt();
      }
    }
  }
}

void loop() {
  checkAndConnectWifi();
  checkAndConnectMqtt();

  if (mySwitch.available()) {
    unsigned long value = mySwitch.getReceivedValue();
    if (value == 0) {
      if (debug)
        Serial.println("Unknown encoding");
    } else {
      mqttClient.publish(codeEventTopic.c_str(), String(value).c_str());
      if (debug) {
        Serial.print("code received ");
        Serial.println(value);
      }
    }
    mySwitch.resetAvailable();
  }

  if (!queue.isEmpty()) {
    CodeQueueItem item = queue.pop();

    if (debug)
      Serial.println("sending code: " + String(item.code) + " length: " + String(item.length) + " protocol: " + String(item.protocol) + " repeatTransmit: " + String(item.repeatTransmit));

    mySwitch.setProtocol(item.protocol);
    mySwitch.setRepeatTransmit(item.repeatTransmit);
    mySwitch.send(item.code, item.length);

    if (debug)
      Serial.println("sent code: " + String(item.code) + " length: " + String(item.length) + " protocol: " + String(item.protocol) + " repeatTransmit: " + String(item.repeatTransmit));

    int queueCount = queue.count();
    mqttClient.publish(queueLengthTopic.c_str(), String(queueCount).c_str());

    if (debug)
      Serial.println("Queue count: " + String(queueCount));
  }

  mqttClient.loop();
  drd->loop();
}
