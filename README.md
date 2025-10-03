# LoRa

This umbrella project contains several components utilizing LoRa for long-range, small-payload
communication for battery-driven applications.

## CellLoRa

This edge application collects atmospheric measurements (temperature, humidity) at hourly intervals
and sends a small data packet through LoRa. When not performing measurements, the module enters
a deep sleep state that consumes a few &mu;A of power. The module is powered by 3 AA batteries in series,
delivering between 3.5 and 5.0V, feeding a down-converter to 3.3V suitable for the electronics.
At each measurement time, the output voltage of the batteries is also measured,
so that a user can anticipate a low battery condition.

The CPU is a ESP32-C3 module, and atmospheric measurement is done with a AHT10.
Battery voltage is measured with a simple 10:1 resistor bridge connected to one of the internal
ADCs of the ESP32-C3. LoRa is provided by a Ra-02 module from AI-Thinker, which uses a SX1278
and is optimized for 433MHz. A relay and transistor is used to control power supply to it.

The system has been tested to run for several months on a set of 3 batteries.
The code has been optimized for reliability, doing 3 re-sends at maximum power, very low bandwidth
and high spread. Depending on the environment and the application, the reliability can be reduced
hence reducing power draw and extending battery life.

## LoRa2MQTT

This bridge application uses a ESP32-S2 with the same SX1280-based LoRa module.
It listens on the LoRa band for messages from edge applications, and takes appropriate action.
In the case of the CellLora, the incoming packet is parsed and the three measurements
(temperature, humidity, battery voltage) are bundled into a MQTT message.
Link to the MQTT broker goes through normal WiFi.

Additional edge devices can be added through the `Vector` array.
Some examples (`BalLoRa` and `Test`) have been provided for reference.

## Home Assistant

MQTT is used as the bridge for Home Assistant. A device named "CellLora" is self-registered
using the three configuration messages `temp.config`, `humi.config`, `batt.config`.
Use the `config.sh` script to run the self-registration, after the MQTT integration
has been enabled on Home Assistant.

After installing `mosquitto` and `mosquitto-client`,
create a password file:

```
mosquitto_passwd -c mosquitto.pwd <user>
```

then move the file `mosquitto.pwd` et `/etc/mosquitto`.

Add the following lines to `/etc/mosquitto/mosquitto.conf`:

```
per_listener_settings true
listener 1883
password_file /etc/mosquitto/mosquitto.pwd
```

then restart `mosquitto`.

Enable the MQTT integration of Home Assistant using the user/password defined above.

## About LoRa

LoRa, as its name implies, is optimized for long-range, low-bandwidth, broadcast communication
over unlicensed spectrum. Depending on geography, spectrum in the 433MHz, 915MHz, or 2.4GHz range
can be used. The lower frequencies typically allow better penetration while higher frequencies
allow higher bandwidth. In out *CellLoRa* application, the system has been tested to push data
from am enclosed cellar in the 2nd basement up to an appartement on the 5th floor of a concrete building.
