****************************************************************
This repository is Elwood Downey's final installment of the 
Autonomous Satellite Tracker ver. 2025081202, as posted on his
Clearsky Institute page. 

I made the following changes to operationally test the code on 
a Huzzah 8266 MCU. 

Webpage.cpp:
Added: Lines 203,204,205, 206, 1204, and modified line 1203 to 
       access TLE files from AMSAT or an internal server that 
       you may wish to set up. No other files were altered.

His notes below, remain intact.
****************************************************************



AutoSatTracker is an ESP8266 sketch that performs completely autonomous Earth satellite tracking.
All control and monioring is done over a wifi with phone, tablet or host computer via a web browser so a 
separate App is not required.

The system requires the following equipment:

    [] ESP8266: Adafruit Huzzah https://www.adafruit.com/product/2821

    [] GPS module for time and location: Adafruit https://www.adafruit.com/product/746

    [] I2C 9 DOF spatial sensor attached to the antenna to measure where it is pointed:
	Adafruit https://www.adafruit.com/product/2472

    [] PWM servo I2C controller: Adafruit https://www.adafruit.com/product/815

    [] 2D gimbal to point the antenna moved by two hobby servo motors:
	I used parts from https://www.servocity.com/

The main program is AutoSatTracker-ESP.ino which uses the following classes:

    Gimbal		control the two servos
    Circum		manage time and location including GPS
    Sensor		read the spatial sensor
    Target		compute the satellite location
    Webpage		display and update the web page

Also required are the following Arduino IDE libraries:
    
    https://github.com/mikalhart/TinyGPS
    https://github.com/adafruit/Adafruit_BNO055/archive/master.zip
    https://github.com/adafruit/Adafruit_Sensor/archive/master.zip
    https://github.com/adafruit/Adafruit-PWM-Servo-Driver-Library
    https://github.com/esp8266/Arduino

    Helpful: https://learn.adafruit.com/adafruit-all-about-arduino-libraries-install-use


How it works:

The main loop() in AutoSetTracker.ino just polls for ethernet and GPS activity then updates the
tracking. Webpage::checkEthernet() checks for a new client connection and replies depending on
the URL they request. The default "/" returns the main web page. After rendering the page, the browser
starts an infinite loop issuing XMLHttpRequests for "/getvalues.txt" which returns many NAME=VALUE pairs.
In most of these, the NAME is the DOM id of the page element to display the VALUE. A few NAMEs,
such as T_TLE, require special handling. Page controls also issue an XMLHttpRequest POST method with
a new NAME=VALUE pair. These pairs are passed to each subsystem for action.

Getting connected:

When first booted the ESP tries to connect to the last known WiFi station using the last IP it used.
If this fails, the ESP changes to Access Point mode as SSID SatTracker. Connecting to this and
browsing to 192.168.10.10 brings up a web page allowing entry of info needed for connecting to your
WiFi station. After Submitting this page, ESP again tries to connect to a WiFi station. This process
repeats until a station connection is successful.

Let me know what you think.

73, Elwood, WB0OEW
ecdowney@clearskyinstitute.com



=====================================================================================================
The P13 code as used here was written by Mark VandeWettering, K6HX, as part of his angst project. See
his blog postings at http://brainwagon.org/the-arduino-n-gameduino-satellite-tracker:



                   _   		The Arduino n' Gameduino
 __ _ _ _  __ _ __| |_ 			Satellite Tracker
/ _` | ' \/ _` (_-<  _|
\__,_|_||_\__, /__/\__|		Written by Mark VandeWettering, K6HX, 2011
          |___/        		brainwagon@gmail.com


Angst is an application that I wrote for the Arduino and Gameduino. It
is being distributed under the so-called "2-class BSD License", which I
think grants potential users the greatest possible freedom to integrate
this code into their own projects. I would, however, consider it a great
courtesy if you could email me and tell me about your project and how
this code was used, just for my own continued personal gratification.

Angst includes P13, a port of the Plan 13 algorithm, originally written
by James Miller, G3RUH and documented here:

	http://www.amsat.org/amsat/articles/g3ruh/111.html

Other implementations exist, even for embedded platforms, such as
the qrpTracker library of Bruce Robertson, VE9QRP and G6LVB's PIC
implementation that is part of his embedded satellite tracker. My own
code was ported from a quick and dirty Python implementation of my own,
and retains a bit of the object orientation that I imposed in that code.

