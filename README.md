# WoL_Device

This project uses an ESP32 dev board module as an endpoint to send a Wake-On-Lan (WoL) packet to a device on the local area network. It is accessed by running as an MQTT client connected to a cloud broker.

## Requirements
This requires that the device to be woken is connected to the router via wired Ethernet. Without wired Ethernet, the network interface card will be powered off when the computer is shut down, so a WoL packet being broadcasted will not power on the device.

## Usage
This project is configured to run with the ESP-IDF extension for VSCode.
1. In the ESP-IDF extension, select the ESP-IDF version, flash method (UART), the port to use, and the Espressif device target.
 
2. Run idf.py menuconfig and edit the menu named "WoL Device Config." Here you can enter WIFI_SSID, WIFI_PASS, MAC_ADDR, BROKER_URI, MQTT_USER, MQTT_PASS. The MAC_ADDR can be found by running ipconfig /all on the targeted device. This project is configured to authenticate to the MQTT broker using a username and password.
  
3. In order to publish messages to the MQTT broker, use an app like MQTTAnalyzer, configure the broker URL and authentication, and send the wake message to the appropriate topic. In this project, the topic is "pc," and the message is "wake," but this can be changed in the code.

## Additional Functionality
In this project, a GPIO pin is configured to remotely force a hardware reset on the PC. Other GPIO functionalities can be implemented depending on the specific application, following a similar structure to the GPIO config setup and use in event functions.
