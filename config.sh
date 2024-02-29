#!/usr/bin/bash

mosquitto_pub -t homeassistant/sensor/CellLora/config -f temp.config
mosquitto_pub -t homeassistant/sensor/CellLora/config -f humi.config
mosquitto_pub -t homeassistant/sensor/CellLora/config -f batt.config
