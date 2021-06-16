/**
 * IotWebConf04UpdateServer.ino -- IotWebConf is an ESP8266/ESP32
 *   non blocking WiFi/AP web configuration library for Arduino.
 *   https://github.com/prampec/IotWebConf 
 *
 * Copyright (C) 2020 Balazs Kelemen <prampec+arduino@gmail.com>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/**
 * Example: Update Server
 * Description:
 *   In this example we will provide a "firmware update" link in the
 *   config portal.
 *   (See ESP8266 ESP8266HTTPUpdateServer examples 
 *   to understand UpdateServer!)
 *   (ESP32: HTTPUpdateServer library is ported for ESP32 in this project.)
 *   (See previous examples for more details!)
 * 
 * Hardware setup for this example:
 *   - An LED is attached to LED_BUILTIN pin with setup On=LOW.
 *   - [Optional] A push button is attached to pin D2, the other leg of the
 *     button should be attached to GND.
 */


#include <MQTT.h>
#include "FastLED.h"
#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
// UpdateServer includes
#ifdef ESP8266
# include <ESP8266HTTPUpdateServer.h>
#elif defined(ESP32)
// For ESP32 IotWebConf provides a drop-in replacement for UpdateServer.
# include <IotWebConfESP32HTTPUpdateServer.h>
#endif

#ifdef ESP8266
String ChipId = String(ESP.getChipId(), HEX);
#elif ESP32
String ChipId = String((uint32_t)ESP.getEfuseMac(), HEX);
#endif

#define FWVERSION "2"


// ------------------ Defines for Wifi -----------------

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
String thingName = "WarpCore_"+ ChipId;

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "12345678";

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "mqt3"

// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to buld an AP. (E.g. in case of lost password)
#define CONFIG_PIN D2

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN

#define STRING_LEN 128

// ------------------ Defines for WarpCore -----------------

// For led chips like Neopixels, which have a data line, ground, and power, you just
// need to define DATA_PIN.  For led chipsets that are SPI based (four wires - data, clock,
// ground, and power), like the LPD8806, define both DATA_PIN and CLOCK_PIN
#define DATA_PIN 1
#define CLOCK_PIN 2
#define SerialSpeed 115200

// How are the LEDs distributed?
#define SegmentSize 5		// How many LEDs in each "Magnetic Constrictor" segment
#define TopLEDcount 10		// LEDs above the "Reaction Chamber"
#define ReactionLEDcount 3	// LEDs inside the "Reaction Chamber"
#define BottomLEDcount 15	// LEDs below the "Reaction Chamber"

// Default Settings
#define DefaultWarpFactor 2	// 1-9
#define DefaultMainHue 160	// 1-255	1=Red 32=Orange 64=Yellow 96=Green 128=Aqua 160=Blue 192=Purple 224=Pink 255=Red
#define DefaultSaturation 255	// 1-255
#define DefaultBrightness 160	// 1-255
#define DefaultPattern 1	// 1-5		1=Standard 2=Breach 3=Rainbow 4=Fade 5=Slow Fade
#define RateMultiplier 2  // 

// ------------------ DO NOT EDIT BELOW THIS LINE -----------------

// -- Method declarations.
void handleRoot();
void handleSettings();
void mqttMessageReceived(String &topic, String &payload);
bool connectMqtt();
bool connectMqttOptions();
void mqttPublishAll();
// -- Callback methods.
void wifiConnected();
void configSaved();
bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper);


DNSServer dnsServer;
WebServer server(80);
#ifdef ESP8266
ESP8266HTTPUpdateServer httpUpdater;
#elif defined(ESP32)
HTTPUpdateServer httpUpdater;
#endif
WiFiClient net;
MQTTClient mqttClient;

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];

IotWebConf iotWebConf(thingName.c_str(), &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);

// -- You can also use namespace formats e.g.: iotwebconf::ParameterGroup
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT configuration");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("MQTT user", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("MQTT password", "mqttPass", mqttUserPasswordValue, STRING_LEN);

bool needMqttConnect = false;
bool needReset = false;
int pinState = HIGH;
unsigned long lastReport = 0;
unsigned long lastMqttConnectionAttempt = 0;

//variables for warpCore:
// How many LEDs in your strip?
#define NUM_LEDS ((TopLEDcount+ReactionLEDcount+BottomLEDcount))		// Total number of LEDs
byte Rate = RateMultiplier * DefaultWarpFactor;    // how fast are the leds fading
byte warp_factor = DefaultWarpFactor;
byte hue = DefaultMainHue;
byte MainHue = DefaultMainHue;
byte ReactorHue = DefaultMainHue;
byte saturation = DefaultSaturation;
byte brightness = DefaultBrightness;
byte pattern = DefaultPattern;


// Define the array of LEDarray
CRGB LEDarray[NUM_LEDS];

void setup() 
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting up...");
  Serial.print("Firmware Version: ");
  Serial.print(FWVERSION);
  Serial.print(" Builddate: ");
  Serial.print(__DATE__);
  Serial.println(__TIME__);


  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addParameterGroup(&mqttGroup);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);



  // -- Define how to handle updateServer calls.
  iotWebConf.setupUpdateServer(
    [](const char* updatePath) { httpUpdater.setup(&server, updatePath); },
    [](const char* userName, char* password) { httpUpdater.updateCredentials(userName, password); });

  // -- Initializing the configuration.
  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
  }
  // reduce this for debugging: 
  iotWebConf.setApTimeoutMs(2000);

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);

  Serial.println("Ready.");

// Fast LED Library: 
  FastLED.addLeds<NEOPIXEL, DATA_PIN>(LEDarray, NUM_LEDS);
//	FastLED.addLeds<WS2801,DATA_PIN, CLOCK_PIN, RGB>(LEDarray,NUM_LEDS);
//	FastLED.setCorrection(Typical8mmPixel);	// (255, 224, 140)
//	FastLED.setCorrection(TypicalSMD5050);	// (255, 176, 240)
	FastLED.setCorrection( CRGB( 255, 200, 245) );
	FastLED.setMaxPowerInVoltsAndMilliamps(5,1000);
	FastLED.setBrightness(brightness);
}

void incrementReactorHue() {
	if (MainHue == 255) {
		ReactorHue = 0;
	} else {
		ReactorHue++;
	}
}

void incrementMainHue() {
	if (MainHue == 255) {
		MainHue = 0;
	} else {
		MainHue++;
	}
}

void incrementHue() {
	incrementMainHue();
	incrementReactorHue();
}


// chase function for WarpCore

#define PulseLength SegmentSize*2
//#define maxChases max(TopLEDcount,BottomLEDcount)/PulseLength+1*PulseLength
#define maxChases (max(TopLEDcount,BottomLEDcount)/PulseLength+1)*PulseLength
#define TopDiff TopLEDcount-BottomLEDcount
#define TopLEDtotal TopLEDcount+ReactionLEDcount
void chase(bool Rainbow, bool Fade, bool SlowFade) {
	static int Pulse;
  if (Pulse == PulseLength-1) {     // loop through pulse every cycle
		Pulse = 0;
		if (SlowFade == true) {
			incrementHue();
		}
	} else {
		Pulse++;
	}
	if (Fade == true) {
		incrementHue();
	}
	// Ramp LED brightness
	for(int value = 32; value < 512; value = value + Rate) {
    if(value > 255){
      value = 255;
    }
		if (Rainbow == true) {
			incrementHue();
		}
		// Set every Nth LED
		for(int chases = 0; chases < maxChases ; chases = chases + PulseLength) {
			byte Top =  Pulse + chases;
			byte Bottom = NUM_LEDS + abs(TopDiff) - (Pulse + chases) - 1;
			if (Top < TopLEDtotal) {
				LEDarray[Top] = CHSV(MainHue, saturation, value);
			}
			if (Bottom > TopLEDcount && Bottom < NUM_LEDS){
				LEDarray[Bottom] = CHSV(MainHue, saturation, value);
			}
		}
		// Keep reaction chamber at full brightness even though we chase the leds right through it
		for (int reaction = 0; reaction < ReactionLEDcount; reaction++) {
			LEDarray[TopLEDcount + reaction] = CHSV(ReactorHue, saturation, 255);
		}
    FastLED.show();					// Show set LEDs
		//fill_solid(LEDarray, NUM_LEDS, CRGB::Black);
    //fadeToBlackBy( LEDarray, NUM_LEDS, (Rate*0.5));	// Dim all LEDs by Rate/2
    fadeToBlackBy( LEDarray, NUM_LEDS, (Rate*0.8));	// Dim all LEDs by Rate*2

    if(value==255){
      break;    // out of the "value" loop
    }
	}
}

// MAIN LOOP 
void loop() 
{
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  mqttClient.loop();
  
  if (needMqttConnect)
  {
    if (connectMqtt())
    {
      needMqttConnect = false;
    }
  }
  else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected()))
  {
    Serial.println("MQTT reconnect");
    connectMqtt();
  }

  if (needReset)
  {
    Serial.println("Rebooting after 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }

  // unsigned long now = millis();
  // if ((500 < now - lastReport) && (pinState != digitalRead(CONFIG_PIN)))
  // {
  //   pinState = 1 - pinState; // invert pin state as it is changed
  //   lastReport = now;
  //   Serial.print("Sending on MQTT channel '/test/status' :");
  //   Serial.println(pinState == LOW ? "ON" : "OFF");
  //   mqttClient.publish("/test/status", pinState == LOW ? "ON" : "OFF");
  // }

  switch(pattern)
  {
    case 1: 	ReactorHue = MainHue;
	            chase(false,false,false);
              break;
    case 2:   {
    	        byte breach_diff = 255 - hue;
	            byte transition_hue = hue + (breach_diff/2);
	            if (ReactorHue < 255) {
                incrementReactorHue();
              }
              if (ReactorHue > transition_hue && MainHue < 255) {
                incrementMainHue();
              }
              if (ReactorHue >= 255 && MainHue >= 255) {
                MainHue = hue;
                ReactorHue = MainHue + 1;
              }
              Rate = (((ReactorHue - MainHue) / (breach_diff / 9) + 1) * RateMultiplier);
              //warpFactor = Rate / RateMultiplier; // why?!?!
              chase(false,false,false);
              } 
              break;
    case 3: chase(true, false, false);
            break;
    case 4: chase(false, true, false);
            break;
    case 5: chase(false, false, true);
            break;
    default: pattern = DefaultPattern;
  }

}

const char index_html_top[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WarpCore Web Server</title>
  <style>
    html {font-family: Arial; display: inline-block; text-align: center;}
    h2 {font-size: 2.3rem;}
    p {font-size: 1.9rem;}
    body {max-width: 400px; margin:0px auto; padding-bottom: 25px;}
    .slider { -webkit-appearance: none; margin: 14px; width: 360px; height: 25px; background: #FFD65C;
      outline: none; -webkit-transition: .2s; transition: opacity .2s;}
    .slider::-webkit-slider-thumb {-webkit-appearance: none; appearance: none; width: 35px; height: 35px; background: #003249; cursor: pointer;}
    .slider::-moz-range-thumb { width: 35px; height: 35px; background: #003249; cursor: pointer; } 
  </style>
</head>
<body>
)rawliteral";

const char index_html_bottom[] PROGMEM = R"rawliteral(
<script>
function updateSlider(element, display, setting) {
  var sliderValue = element.value;
  document.getElementById(display).innerHTML = sliderValue;
  console.log(sliderValue);
  var xhr = new XMLHttpRequest();
  xhr.open("GET", "/settings?"+setting+"="+sliderValue, true);
  xhr.send();
}
function updatePattern(setting) {
  console.log(setting);
  var xhr = new XMLHttpRequest();
  xhr.open("GET", "/settings?pattern="+setting, true);
  xhr.send();
}
</script>
</body>
</html>
)rawliteral";

/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  // "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send_P(200, "text/html", index_html_top);
  
  String s ="<h1>WarpCore Control</h1>";
  s += "<table><tr>";
  s += "<td><p>Brightness:</p></td>";
  s += "<td><input type=\"range\" onchange=\"updateSlider(this,'brightnessSliderValue','brightness')\" id=\"brightnessSlider\" min=\"0\" max=\"255\" value=\"" + String(brightness) + "\" step=\"1\" class=\"slider\"></td> ";
  s += "<td><p><span id=\"brightnessSliderValue\">"+ String(brightness) + "</span></p></td>";
  s += "</tr><tr>";
  s += "<td><p>Saturation:</p></td>";
  s += "<td><input type=\"range\" onchange=\"updateSlider(this,'saturationSliderValue','saturation')\" id=\"saturationSlider\" min=\"0\" max=\"255\" value=\"" + String(saturation) + "\" step=\"1\" class=\"slider\"></td> ";
  s += "<td><p><span id=\"saturationSliderValue\">"+ String(saturation) + "</span></p></td>";
  s += "</tr><tr>";
  s += "<td><p>Hue:</p></td>";
  s += "<td><input type=\"range\" onchange=\"updateSlider(this,'hueSliderValue','hue')\" id=\"hueSlider\" min=\"0\" max=\"255\" value=\"" + String(hue) + "\" step=\"1\" class=\"slider\"></td> ";
  s += "<td><p><span id=\"hueSliderValue\">"+ String(hue) + "</span></p></td>";
  s += "</tr><tr>";
  s += "<td><p>WarpSpeed:</p></td>";
  s += "<td><input type=\"range\" onchange=\"updateSlider(this,'warpSpeedSliderValue','warpFactor')\" id=\"warpSpeedSlider\" min=\"1\" max=\"9\" value=\"" + String(warp_factor) + "\" step=\"1\" class=\"slider\"></td> ";
  s += "<td><p><span id=\"warpSpeedSliderValue\">"+ String(warp_factor) + "</span></p></td>";
  s += "</tr><tr>";
  s += "<td colspan=\"3\">";
  s += "<input type=\"button\" onclick=\"updatePattern('1')\" id=\"StandardButton\" value=\"Standard\" class=\"button\">";
  s += "<input type=\"button\" onclick=\"updatePattern('2')\" id=\"CoreBreachButton\" value=\"Core Breach\" class=\"button\">";
  s += "<input type=\"button\" onclick=\"updatePattern('3')\" id=\"RainbowdButton\" value=\"Rainbow\" class=\"button\">";
  s += "<input type=\"button\" onclick=\"updatePattern('4')\" id=\"FadeButton\" value=\"Fade\" class=\"button\">";
  s += "<input type=\"button\" onclick=\"updatePattern('5')\" id=\"SlowFadeButton\" value=\"Slow Fade\" class=\"button\">";
  s += "</td>";
  s += "</tr></table><br><br>";

  s += "Go to <a href='config'>configure page</a> to change values. <br>";
  s += "Firmware Version: ";
  s += FWVERSION;
  s += " - ";
  //s += "Date: " ;
  s += __DATE__;
  s += " ";
  s += __TIME__;

  server.sendContent(s);
  server.sendContent_P(index_html_bottom);
  server.sendContent("");

  // server.send(200, "text/html", s);
}


/**
 * Handle web requests to "/settings" path.
 */
void handleSettings()
{
  if(server.hasArg("pattern")){
    int t = server.arg("pattern").toInt();
    if(t < 1) t=1;
    if(t > 5) t=5;
    pattern=t;
    Serial.print("pattern = " );
    Serial.println( pattern );
  }
  if(server.hasArg("brightness")){
    int t = server.arg("brightness").toInt();
    if(t < 0)   t=0;
    if(t > 255) t=255;
    brightness=t;
    FastLED.setBrightness(brightness);
    Serial.print("brightness = " );
    Serial.println( brightness );
  }
  if(server.hasArg("hue")){
    int t = server.arg("hue").toInt();
    if(t < 0)   t=0;
    if(t > 255) t=255;
    hue=t;
    MainHue = hue;
    ReactorHue = hue;
    Serial.print("hue = " );
    Serial.println( hue );
  }
  if(server.hasArg("saturation")){
    int t = server.arg("saturation").toInt();
    if(t < 0)   t=0;
    if(t > 255) t=255;
    saturation=t;

    Serial.print("saturation = " );
    Serial.println( saturation );
  }
  if(server.hasArg("warpFactor")){
    int t = server.arg("warpFactor").toInt();
    if(t < 1) t=1;
    if(t > 9) t=9;
    warp_factor=t;
    Rate = RateMultiplier * warp_factor;
    Serial.print("warpFactor = " );
    Serial.println( warp_factor );
  }
  server.send(200, "text/plain", "Thanks!");
  mqttPublishAll();
}

void wifiConnected()
{
  needMqttConnect = true;
}

void configSaved()
{
  Serial.println("Configuration was updated.");
  needReset = true;
}

bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper)
{
  Serial.println("Validating form.");
  bool valid = true;

  int l = webRequestWrapper->arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }

  return valid;
}

bool connectMqtt() {
  unsigned long now = millis();
  if (1000 > now - lastMqttConnectionAttempt)
  {
    // Do not repeat within 1 sec.
    return false;
  }
  Serial.println("Connecting to MQTT server...");
  if (!connectMqttOptions()) {
    lastMqttConnectionAttempt = now;
    return false;
  }
  Serial.println("Connected!");

//  mqttClient.subscribe("/test/action");
  mqttClient.subscribe("/WarpCore/"+ String(iotWebConf.getThingName()) +"/warpFactor");
  mqttClient.subscribe("/WarpCore/"+ String(iotWebConf.getThingName()) +"/hue");
  mqttClient.subscribe("/WarpCore/"+ String(iotWebConf.getThingName()) +"/saturation");
  mqttClient.subscribe("/WarpCore/"+ String(iotWebConf.getThingName()) +"/brightness");
  mqttClient.subscribe("/WarpCore/"+ String(iotWebConf.getThingName()) +"/pattern");

  mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/FWVersion",    String(FWVERSION));
  mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/FWDate",    String(__DATE__)+ " " +String(__TIME__));

  return true;
}

/*
// -- This is an alternative MQTT connection method.
bool connectMqtt() {
  Serial.println("Connecting to MQTT server...");
  while (!connectMqttOptions()) {
    iotWebConf.delay(1000);
  }
  Serial.println("Connected!");
  mqttClient.subscribe("/test/action");
  return true;
}
*/

bool connectMqttOptions()
{
  bool result;
  if (mqttUserPasswordValue[0] != '\0')
  {
    result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue, mqttUserPasswordValue);
  }
  else if (mqttUserNameValue[0] != '\0')
  {
    result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue);
  }
  else
  {
    result = mqttClient.connect(iotWebConf.getThingName());
  }
  return result;
}

void mqttMessageReceived(String &topic, String &payload)
{
  Serial.println("Incoming: " + topic + " - " + payload);

  if(topic.indexOf("pattern")!=-1)
  { 
    pattern = payload.toInt();
    Serial.print("pattern = ");
    Serial.println( pattern );
  }

  if(topic.indexOf("hue")!=-1)
  {
    hue = payload.toInt();
    MainHue = hue;
    ReactorHue = hue;
    Serial.print("hue = ");
    Serial.println( hue );
  }

  if(topic.indexOf("brightness")!=-1)
  { 
    brightness = payload.toInt();
    FastLED.setBrightness(brightness);
    Serial.print("brightness = " );
    Serial.println( brightness );
  }

  if(topic.indexOf("saturation")!=-1)
  {
    saturation = payload.toInt();
    Serial.print("saturation = ");
    Serial.println( saturation );
  }

  if(topic.indexOf("warpFactor")!=-1)
  {
    warp_factor = payload.toInt();
    Rate = RateMultiplier * warp_factor;
    Serial.print("warpFactor = ");
    Serial.println( warp_factor );
  }
  mqttPublishAll();
}

void mqttPublishAll()
{
  if(mqttClient.connected())
  {
    mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/WarpFactor", String(warp_factor));
    mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/hue",        String(hue));
    mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/saturation", String(saturation));
    mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/brightness", String(brightness));
    mqttClient.publish("/WarpCore/"+ String(iotWebConf.getThingName())+ "/status/pattern",    String(pattern));
  }
}


