# LoRa

## Mosquitto 

### Password file

```
mosquitto_passwd -c mosquitto.pwd <user>
```

Then move the file `mosquitto.pwd` et `/etc/mosquitto`.

### mosquitto.conf

```
per_listener_settings true
listener 1883
password_file /etc/mosquitto/mosquitto.pwd
```

## Home Assistant

Enable the MQTT integration using the user/password defined above.

