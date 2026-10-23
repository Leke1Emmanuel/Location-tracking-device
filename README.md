# Location Tracking Device

This project eemployed the use of an ESP32 D0WD V3 module breakout board with a SIM800L GSM module  and a NEO-6M GPS module which would send data to a render webserver and from there published to a software application(I only built the hardare + server), a PushButton and an LED were also used. 

1. The ESP32 was configured in STA and AP modes where it would create its own wifi network(AP /hotspot) and then allow another hotspot's name and password be configured and saved as well as a deviceId and deviceKey native to the software and generated for each individual tracker after which it tries to connect to the saved HOTSPOT network IF AND ONLY IF it isnt able to connect to internet using the GSM module because it always takes priority on every start-up.

2. The  GSM module was initially used to send the location data gotten from the GPS module to the webserver but due to TLS and HTTP-to-HTTPS redirects, we decided it would only serve as internet connection for the ESP32 whose work has changed from just being a processor to also perfoming the TLS handshake.

3. The ESP32 can use either wifi or the internet connection from GSM as methods to connect to the internet and they each serve as backup for eachother if an issue comes up on one.  

4. The GPS module only serves a location sourcing device from nearby sattelites.

5. The pushbutton is used to trigger an sos signal that would send last known location to the websever as well as trigger LED to blink twice in 1s if it successfully sends it. It'll blink once in 2s if it couldnt send the sos signal after 5 attempts due to network failure.

# Things to note:
1. The GPS module must have the ceramic surface of its antenna facing the sky preferably in an open space or close to the window and wait for upto 60s~ coldstart after which the red/green LED starts blinking which means it has connected to at least 4 Satellites. 
2. The GSM module has 3 modes depending on its red LED blinking speed:
  - Once every 3s means it has successfully recognized a working sim in its module
  - Once every 1s means it hasnt found any sim or couldnt connect to it 
  - Thrice every 1s means it is actively trying to push data over the internet
3. After the 5 attempts are over and sos signal isnt sent(LED blinks once in 2s), it forgets that sos signal was ever sent(meaning youd have to trigger it again) and during the 5 attempts process, the whole data transfer system is basically held down until complete so you cant trigger WIFI captive portal to configure hotspot as well as deviceId and deviceKey.  