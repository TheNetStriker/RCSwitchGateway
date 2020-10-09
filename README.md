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
## OTA Update
To enable OTA update just set **default_envs** to **release** and add an ota.txt file containing the ip of the device and a password on separate lines. The **ota.py** file will automatically add the required configuration for the OTA upload.
## Hardware
In the **Eagle** folder you can find Eagle and Gerber files for a feather board to connect a MX-05V receiver and an FS1000A sender to the Adafruit Huzzah32.
## MQTT commands and events
The MQTT messages follow the [Homie standard](https://homieiot.github.io/) and the device will be automatically autodetected on controllers that support this standard.
### Sender
Commands are sent as json text.  
**homie/hostname/sender/send_type_a**: Command to send a type A RC Signal with the following settings:
`{"group": "11111", "device": "11111", "repeatTransmit": 5, "switchOnOff": true}`
**homie/hostname/sender/send**: Command to send a custom signal with the following attributes:
`{"code": 1234, "codeLength": 24, "protocol": 1, "repeatTransmit": 5 }`
### Receiver
The receiver is just sending plain numbers.  
**homie/hostname/receiver/queue_length**: Length of the current queue of signals to be sent.  
**homie/rcswitch01/receiver/code_received**: Received code event
### System
The device also sends some system values.
**homie/rcswitch01/system/rssi**: The device send's the wifi signal strength every minute to this topic. 
**homie/rcswitch01/system/log**: At the moment this just send's an "Startup" message when the device started. This helps to find out if the device crashed at some point.