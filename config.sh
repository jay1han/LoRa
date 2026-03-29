#!/usr/bin/bash

mosquitto_pub -u guest -P invitation -t homeassistant/sensor/CellLora_Temperature/config -f temp.config
mosquitto_pub -u guest -P invitation -t homeassistant/sensor/CellLora_Humidity/config -f humi.config
mosquitto_pub -u guest -P invitation -t homeassistant/sensor/CellLora_Battery/config -f batt.config
