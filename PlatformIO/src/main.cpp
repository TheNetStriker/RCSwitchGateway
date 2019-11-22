#include "Arduino.h"
#include "RCSwitch.h"
#include "Defines.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <MQTT.h>
#include <ESPmDNS.h>
#include <QueueList.h>
#include <ArduinoJson.h>

WiFiClient net;
MQTTClient client;
RCSwitch mySwitch = RCSwitch();

int receivePin = 15;
int transmitPin = 32;

const String sendTypeATopic = String("/") + HOSTNAME + String("/sender/sendtypea");
const String sendTopic = String("/") + HOSTNAME + "/sender/send";
const String queueLengthTopic = String("/") + HOSTNAME + "/queue/length";
const String codeEventTopic = String("/") + HOSTNAME + "/events/codereceived";

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

void connectMqtt() {
  Serial.print("checking wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }

  Serial.print("\nconnecting mqtt...");
  while (!client.connect(HOSTNAME)) {
    int errorCode = client.lastError();
    Serial.print(" " + String(errorCode) + " ");
    delay(1000);
  }

  Serial.println("\nconnected!");

  client.subscribe(sendTypeATopic);
  client.subscribe(sendTopic);
}

void messageReceived(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  if (queue.count() > maxQueueCount) {
    if (debug)
      Serial.println("Error: Send queue is full!");
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload);
  JsonObject json = doc.as<JsonObject>();
  
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    return;
  }

  if (topic == sendTypeATopic) {
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
  } else if (topic == sendTopic) {
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

void setup() {
  Serial.begin(115200);

  mySwitch.enableReceive(receivePin);
  mySwitch.enableTransmit(transmitPin);
  mySwitch.setProtocol(2);
  mySwitch.setRepeatTransmit(5);

  WiFi.disconnect(true, true);
  WiFi.begin(WLAN_SSID, WLAN_PASSWORD);
  Serial.print("Connecting to ");
  Serial.println(WLAN_SSID);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (WiFi.status() == WL_CONNECT_FAILED) {
      delay(5000);
      Serial.println("");
      Serial.println("Connect failed");
      WiFi.begin(WLAN_SSID, WLAN_PASSWORD);
      Serial.print(".");
    }
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WLAN_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(HOSTNAME)) {
    Serial.println("MDNS responder started");
  }

  client.begin(MQTT_BROKER_IP, net);
  client.onMessage(messageReceived);

  connectMqtt();
}

void loop() {
  if (mySwitch.available()) {
    unsigned long value = mySwitch.getReceivedValue();
    if (value == 0) {
      if (debug)
        Serial.println("Unknown encoding");
    } else {
      client.publish(codeEventTopic, String(value));
      if (debug) {
        Serial.print("code received ");
        Serial.println(value);
      }
    }
    mySwitch.resetAvailable();
  }

  if (queue.isEmpty()) {
    delay(10);  // <- fixes some issues with WiFi stability
  } else {
    CodeQueueItem item = queue.pop();

    if (debug)
      Serial.println("sending code: " + String(item.code) + " length: " + String(item.length) + " protocol: " + String(item.protocol) + " repeatTransmit: " + String(item.repeatTransmit));

    mySwitch.setProtocol(item.protocol);
    mySwitch.setRepeatTransmit(item.repeatTransmit);
    mySwitch.send(item.code, item.length);

    if (debug)
      Serial.println("sent code: " + String(item.code) + " length: " + String(item.length) + " protocol: " + String(item.protocol) + " repeatTransmit: " + String(item.repeatTransmit));

    int queueCount = queue.count();
    client.publish(queueLengthTopic, String(queueCount));

    if (debug)
      Serial.println("Queue count: " + String(queueCount));
  }

  client.loop();

  if (!client.connected()) {
    int errorCode = client.lastError();
    Serial.println("MQTT disconnected, error code: " + String(errorCode));
    connectMqtt();
  }
}
