# RCSwitchGateway
## Description
This program uses a modified version of the [rc-switch library](https://github.com/sui77/rc-switch) to control 433 MHZ based switches using MQTT commands over Wifi. It runs on [ESP32](https://www.espressif.com/en/products/hardware/esp32/overview) microcontrollers. (Currently only tested with Adafruit Huzzah32) This program can also detect 433 MHZ signals and send them back via MQTT. Currently it only supports sending **Type A** signals and can also receive commands from e.g. König SAS-SA200 and SAS-SA2002 smoke detectors. Please look at the [RC Switch Wiki](https://github.com/sui77/rc-switch/wiki/Add_New_Remote_Part_1) on how to implement more signals.
## Installation
- Install the ESP32 on the Arduino IDE as described [here](https://randomnerdtutorials.com/installing-the-esp32-board-in-arduino-ide-windows-instructions/).
- Clone the project and create an **Defines.h** file containing the following information:
```cpp
#define WLAN_SSID "MYSSID"
#define WLAN_PASSWORD "MYPASSWORD"
#define MQTT_BROKER_IP "MYIP"
#define HOSTNAME "MYHOSTNAME"
```
## Hardware
In the **Eagle** folder you can find Eagle and Gerber files for a feather board to connect a MX-05V receiver and an FS1000A sender to the Adafruit Huzzah32.
## MQTT commands and events
### Commands
Commands are sent as json text.  
**/hostname/sender/sendtypea**: Command to send a type A RC Signal with the following settings:
`{"group": "11111", "device": "11111", "repeatTransmit": 5, "switchOnOff": true}`
**/hostname/sender/send**: Command to send a custom signal with the following attributes:
`{"code": 1234, "codeLength": 24, "protocol": 1, "repeatTransmit": 5 }`
### Events
Events are just plain numbers.  
**/hostname/queue/length**: Length of the current queue of signals to be sent.  
**/hostname/events/codereceived**: Received code event