/*
 Name:		AmsToMqttBridge.ino
 Created:	3/13/2018 7:40:28 PM
 Author:	roarf
			modified by stenjo for reading Aidon messages while waiting for HAN port to be up to standard
*/


#include <DallasTemperature.h>
#include <OneWire.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "HanReader.h"
#include "Kaifa.h"
#include "Kamstrup.h"
#include "configuration.h"
#include "accesspoint.h"

#define WIFI_CONNECTION_TIMEOUT 30000;
#define TEMP_SENSOR_PIN 5 // Temperature sensor connected to GPIO5
#define LED_PIN 2 // The blue on-board LED of the ESP

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);
long lastTempDebug = 0;

// Object used to boot as Access Point
accesspoint ap;

// WiFi client and MQTT client
WiFiClient *client;
PubSubClient mqtt;

// Object used for debugging
HardwareSerial* debugger = NULL;

// The HAN Port reader, used to read serial data and decode DLMS
HanReader hanReader;

// the setup function runs once when you press reset or power the board
void setup() 
{
	// Uncomment to debug over the same port as used for HAN communication
	debugger = &Serial;
	
	if (debugger) {
		// Setup serial port for debugging
		debugger->begin(9600);
		while (!&debugger);
		debugger->println("Started...");
	}

	// Assign pin for boot as AP
	delay(1000);
	pinMode(0, INPUT_PULLUP);
	
	// Flash the blue LED, to indicate we can boot as AP now
	pinMode(LED_PIN, OUTPUT);
	digitalWrite(LED_PIN, LOW);
	
	// Initialize the AP
	ap.setup(0, Serial);
	
	// Turn off the blue LED
	digitalWrite(LED_PIN, HIGH);

	if (!ap.isActivated)
	{
		setupWiFi();
		delay(10000);
		hanReader.setup(&Serial, 9600, SERIAL_8N1, debugger);
		
		// Compensate for the known Kaifa bug
		hanReader.compensateFor09HeaderBug = (ap.config.meterType == 1);
	}
}

// the loop function runs over and over again until power down or reset
void loop()
{
	// Only do normal stuff if we're not booted as AP
	if (!ap.loop())
	{
		// turn off the blue LED
		digitalWrite(LED_PIN, HIGH);

		// allow the MQTT client some resources
		mqtt.loop();
		delay(10); // <- fixes some issues with WiFi stability

		// Reconnect to WiFi and MQTT as needed
		if (!mqtt.connected()) {
			MQTT_connect();
		}
		else
		{
			// Read data from the HAN port
			readHanPort();
		}
	}
	else
	{
		// Continously flash the blue LED when AP mode
		if (millis() / 1000 % 2 == 0)
			digitalWrite(LED_PIN, LOW);
		else
			digitalWrite(LED_PIN, HIGH);
	}
}

void setupWiFi()
{
	// Turn off AP
	WiFi.enableAP(false);
	
	// Connect to WiFi
	WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
	
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
	}
	
	// Initialize WiFi and MQTT clients
	if (ap.config.isSecure())
		client = new WiFiClientSecure();
	else
		client = new WiFiClient();
	mqtt = PubSubClient(*client);
	mqtt.setServer(ap.config.mqtt, ap.config.mqttPort);

	// Direct incoming MQTT messages
	if (ap.config.mqttSubscribeTopic != 0 && strlen(ap.config.mqttSubscribeTopic) > 0)
		mqtt.setCallback(mqttMessageReceived);

	// Connect to the MQTT server
	MQTT_connect();

	// Notify everyone we're here!
	sendMqttData("Connected!");
}

void mqttMessageReceived(char* topic, unsigned char* payload, unsigned int length)
{
	// make the incoming message a null-terminated string
	char message[1000];
	for (int i = 0; i < length; i++)
		message[i] = payload[i];
	message[length] = 0;

	if (debugger) {
		debugger->println("Incoming MQTT message:");
		debugger->print("[");
		debugger->print(topic);
		debugger->print("] ");
		debugger->println(message);
	}

	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();
	JsonObject& data = root.createNestedObject("data");

		// Get the temperature too
	tempSensor.requestTemperatures();
	float temperature = tempSensor.getTempCByIndex(0);
	data["temp"] = String(temperature);

	// Publish the json to the MQTT server
	char msg[1024];
	root.printTo(msg, 1024);
	mqtt.publish(ap.config.mqttPublishTopic, "/energy/total/1183.87");

	// Do whatever needed here...
	// Ideas could be to query for values or to initiate OTA firmware update
}

void readHanPort()
{
	if (hanReader.read())
	{
		// Flash LED on, this shows us that data is received
		digitalWrite(LED_PIN, LOW);

		// Get the list identifier
		int listSize = hanReader.getListSize();

		switch (ap.config.meterType)
		{
		case 1: // Kaifa
			readHanPort_Kaifa(listSize);
			break;
		case 2: // Aidon
			readHanPort_Aidon(listSize);
			break;
		case 3: // Kamstrup
			readHanPort_Kamstrup(listSize);
			break;
		default:
			debugger->print("Meter type ");
			debugger->print(ap.config.meterType, HEX);
			debugger->println(" is unknown");
			delay(10000);
			break;
		}

		// Flash LED off
		digitalWrite(LED_PIN, HIGH);
	}
}

void readHanPort_Aidon(int listSize)
{

	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();
	JsonObject& data = root.createNestedObject("data");

		// Get the temperature too
	tempSensor.requestTemperatures();
	float temperature = tempSensor.getTempCByIndex(0);
	data["temp"] = String(temperature);
	data["list"] = String((float)listSize);
	data["id"] = hanReader.getString(0, 16);

	// 16-23: Import sum (Wh)
	debugger->print("Forbruk sum (kWh): ");
	//debugger->print(String((float)hanReader.getInt(16,4)/1000));
	char value[1024];
	sprintf(value, "%.2f", (float)hanReader.getInt(16,4)/1000);
	mqtt.publish("ams2mqtt/energy/import/accumulated", value);
	debugger->print(value);

	// 48-51: Import effekt totalt (W)
	debugger->print("  Forbruk totalt (W): ");
	sprintf(value, "%d", hanReader.getInt(48,4));
	mqtt.publish("ams2mqtt/power/import/total", value);
	debugger->print(value);

	// 70-71: Import fase 1 (W)
	debugger->print("  Forbruk fase 1 (W): ");
	sprintf(value, "%d", hanReader.getInt(70,2));
	mqtt.publish("ams2mqtt/power/import/phase1", value);
	debugger->print(value);

	// 74-75: Import fase 2 (W)
	debugger->print("  Forbruk fase 2 (W): ");
	sprintf(value, "%d", hanReader.getInt(74,2));
	mqtt.publish("ams2mqtt/power/import/phase2", value);
	debugger->print(value);

	// 78-79: Import fase 3 (W)
	debugger->print("  Forbruk fase 3 (W): ");
	sprintf(value, "%d", hanReader.getInt(78,2));
	mqtt.publish("ams2mqtt/power/import/phase3", value);
	debugger->println(value);

 
	// 24-31: Eksport sum (Wh)
	debugger->print("Eksport sum (kWh):    ");
	//debugger->print(String((float)hanReader.getInt(24,4)/1000));
	sprintf(value, "%.2f", (float)hanReader.getInt(24,4)/1000);
	mqtt.publish("ams2mqtt/energy/export/accumulated", value);
	debugger->print(value);

	// 52-55: Eksport Effekt Totalt (W)
	debugger->print("  Eksport totalt (W): ");
	sprintf(value, "%d", hanReader.getInt(52,4));
	mqtt.publish("ams2mqtt/power/export/total", value);
	debugger->print(value);


	// 72-73: Eksport fase 1 (W)
	debugger->print("  Eksport fase 1 (W): ");
	sprintf(value, "%d", hanReader.getInt(72,2));
	mqtt.publish("ams2mqtt/power/export/phase1", value);
	debugger->print(value);

	// 76-77: Eksport fase 2 (W)
	debugger->print("  Eksport fase 2 (W): ");
	sprintf(value, "%d", hanReader.getInt(76,2));
	mqtt.publish("ams2mqtt/power/export/phase2", value);
	debugger->print(value);

	// 80-81: Eksport fase 3 (W)
	debugger->print("  Eksport fase 3 (W): ");
	sprintf(value, "%d", hanReader.getInt(80,2));
	mqtt.publish("ams2mqtt/power/export/phase3", value);
	debugger->println(value);

	// 88-89: Strøm fase 1 (A) *10
	debugger->print("Strøm fase 1 (A): ");
	//debugger->print(String((float)hanReader.getInt(88,2)/10));
	sprintf(value, "%.1f", (float)hanReader.getInt(88,2)/10);
	mqtt.publish("ams2mqtt/current/phase1", value);
	debugger->print(value);

	// 90-91: Strøm fase 2 (A) *10
	debugger->print("  Strøm fase 2 (A): ");
	//debugger->print(String((float)hanReader.getInt(90,2)/10));
	sprintf(value, "%.1f", (float)hanReader.getInt(90,2)/10);
	mqtt.publish("ams2mqtt/current/phase2", value);
	debugger->print(value);

	// 92-93: Strøm fase 3 (A) *10
	debugger->print("  Strøm fase 3 (A): ");
	//debugger->println(String((float)hanReader.getInt(92,2)/10));
	sprintf(value, "%.1f", (float)hanReader.getInt(92,2)/10);
	mqtt.publish("ams2mqtt/current/phase3", value);
	debugger->println(value);

	// 82-83: Spenning fase 1 (V) *10
	debugger->print("Spenning fase 1 (V): ");
	//debugger->print(String((float)hanReader.getInt(82,2)/10));
	sprintf(value, "%.1f", (float)hanReader.getInt(82,2)/10);
	mqtt.publish("ams2mqtt/voltage/phase1", value);
	debugger->print(value);

	// 84-85: Spenning fase 2 (V) *10
	debugger->print("  Spenning fase 2 (V): ");
	//debugger->print(String((float)hanReader.getInt(84,2)/10));
	sprintf(value, "%.1f", (float)hanReader.getInt(84,2)/10);
	mqtt.publish("ams2mqtt/voltage/phase2", value);
	debugger->print(value);

	// 86-87: Spenning fase 3 (V) *10
	debugger->print("  Spenning fase 3 (V): ");
	//debugger->println(String((float)hanReader.getInt(86,2)/10));
	sprintf(value, "%.1f", (float)hanReader.getInt(86,2)/10);
	mqtt.publish("ams2mqtt/voltage/phase3", value);
	debugger->println(value);

	// 94-95: Frekvens (Hz) *100
	debugger->print("Frekvens: ");
	sprintf(value, "%.2f", (float)hanReader.getInt(94,2)/100);
	mqtt.publish("ams2mqtt/energy/frequency", value);
	debugger->println(value);

// Ukjente variabler

	// 32-39: Reaktiv energi R+ (Wh)
	debugger->print("Reaktiv energi R+ (Wh):    ");
	sprintf(value, "%.2f", ((float)hanReader.getInt(32,4)/1000));
	mqtt.publish("ams2mqtt/energy/reactive/posacc", value);
	debugger->print(value);

	// 40-47: Reaktiv energi R- (Wh)
	debugger->print(" Reaktiv energi R- (Wh):    ");
	//debugger->print(String((float)hanReader.getInt(40,4)/1000));
	sprintf(value, "%.2f", (float)hanReader.getInt(40,4)/1000);
	mqtt.publish("ams2mqtt/energy/reactive/negacc", value);
	debugger->print(value);


	// 56-59: Reaktiv effekt R+ (W)
	debugger->print("  Reaktiv effekt R+ (W): ");
	sprintf(value, "%d", hanReader.getInt(56,4));
	mqtt.publish("ams2mqtt/power/reactive/rpos", value);
	debugger->print(value);

	// 56-59: Reaktiv effekt R+ (W)
	debugger->print("  Reaktiv effekt R- (W): ");
	sprintf(value, "%d", hanReader.getInt(60,4));
	mqtt.publish("ams2mqtt/power/reactive/rneg", value);
	debugger->print(value);

	// 64-67: Ukjent
	debugger->print("  Ukjent 1: ");
	sprintf(value, "%d", hanReader.getInt(64,2));
	mqtt.publish("ams2mqtt/power/unknown/1", value);
	debugger->print(value);

	// 64-67: Ukjent
	debugger->print("  Ukjent 2: ");
	sprintf(value, "%d", hanReader.getInt(66,2));
	mqtt.publish("ams2mqtt/power/unknown/2", value);
	debugger->print(value);
	// 64-67: Ukjent
	debugger->print("  Ukjent 3: ");
	sprintf(value, "%d", hanReader.getInt(68,2));
	mqtt.publish("ams2mqtt/power/unknown/3", value);
	debugger->println(value);



	data["tPI"] = String((float)hanReader.getInt(16,4)/1000);
	data["tPO"] = hanReader.getInt(20,4);
	data["P1"] = hanReader.getInt(70,2);
	data["P2"] = hanReader.getInt(74,2);
	data["P3"] = hanReader.getInt(78,2);
	data["f"] = String((float)hanReader.getInt(94,2)/100);

	// Publish the json to the MQTT server
	char msg[1024];
	root.printTo(msg, 1024);
	mqtt.publish(ap.config.mqttPublishTopic, msg);

	hanReader.Clear();
}

void WriteAndEmptyBuffer()
{
	debugger->println();
//	debugger->println($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} - Received {gBuffer.Count} (0x{gBuffer.Count:X2}) bytes]");
/*
	int j = 0;
	foreach (var vByte in gBuffer)
	{
		debugger->println(string.Format("{0:X2} ", (int)vByte));

		if (++j % 8 == 0)
			debugger->println(" ");

		if (j % 24 == 0)
			debugger->println();
	}

	debugger->println();
	debugger->println();
*/
	//gBuffer.Clear();
}

void readHanPort_Kamstrup(int listSize)
{
	// Only care for the ACtive Power Imported, which is found in the first list
	if (listSize == (int)Kamstrup::List1 || listSize == (int)Kamstrup::List2)
	{
		if (listSize == (int)Kamstrup::List1)
		{
			String id = hanReader.getString((int)Kamstrup_List1::ListVersionIdentifier);
			if (debugger) debugger->println(id);
		}
		else if (listSize == (int)Kamstrup::List2)
		{
			String id = hanReader.getString((int)Kamstrup_List2::ListVersionIdentifier);
			if (debugger) debugger->println(id);
		}

		// Get the timestamp (as unix time) from the package
		time_t time = hanReader.getPackageTime();
		if (debugger) debugger->print("Time of the package is: ");
		if (debugger) debugger->println(time);

		// Define a json object to keep the data
		StaticJsonBuffer<500> jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();

		// Any generic useful info here
		root["id"] = WiFi.macAddress();
		root["up"] = millis();
		root["t"] = time;

		// Add a sub-structure to the json object, 
		// to keep the data from the meter itself
		JsonObject& data = root.createNestedObject("data");

		// Get the temperature too
		tempSensor.requestTemperatures();
		float temperature = tempSensor.getTempCByIndex(0);
		data["temp"] = temperature;

		// Based on the list number, get all details 
		// according to OBIS specifications for the meter
		if (listSize == (int)Kamstrup::List1)
		{
			data["lv"] = hanReader.getString((int)Kamstrup_List1::ListVersionIdentifier);
			data["id"] = hanReader.getString((int)Kamstrup_List1::MeterID);
			data["type"] = hanReader.getString((int)Kamstrup_List1::MeterType);
			data["P"] = hanReader.getInt((int)Kamstrup_List1::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kamstrup_List1::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kamstrup_List1::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kamstrup_List1::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kamstrup_List1::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kamstrup_List1::VoltageL1);
			data["U2"] = hanReader.getInt((int)Kamstrup_List1::VoltageL2);
			data["U3"] = hanReader.getInt((int)Kamstrup_List1::VoltageL3);
		}
		else if (listSize == (int)Kamstrup::List2)
		{
			data["lv"] = hanReader.getString((int)Kamstrup_List2::ListVersionIdentifier);;
			data["id"] = hanReader.getString((int)Kamstrup_List2::MeterID);
			data["type"] = hanReader.getString((int)Kamstrup_List2::MeterType);
			data["P"] = hanReader.getInt((int)Kamstrup_List2::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kamstrup_List2::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kamstrup_List2::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kamstrup_List2::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kamstrup_List2::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kamstrup_List2::VoltageL1);
			data["U2"] = hanReader.getInt((int)Kamstrup_List2::VoltageL2);
			data["U3"] = hanReader.getInt((int)Kamstrup_List2::VoltageL3);
			data["tPI"] = hanReader.getInt((int)Kamstrup_List2::CumulativeActiveImportEnergy);
			data["tPO"] = hanReader.getInt((int)Kamstrup_List2::CumulativeActiveExportEnergy);
			data["tQI"] = hanReader.getInt((int)Kamstrup_List2::CumulativeReactiveImportEnergy);
			data["tQO"] = hanReader.getInt((int)Kamstrup_List2::CumulativeReactiveExportEnergy);
		}

		// Write the json to the debug port
		if (debugger) {
			debugger->print("Sending data to MQTT: ");
			root.printTo(*debugger);
			debugger->println();
		}

		// Make sure we have configured a publish topic
		if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
			return;

		// Publish the json to the MQTT server
		char msg[1024];
		root.printTo(msg, 1024);
		mqtt.publish(ap.config.mqttPublishTopic, msg);
	}
}


void readHanPort_Kaifa(int listSize) 
{
	// Only care for the ACtive Power Imported, which is found in the first list
	if (listSize == (int)Kaifa::List1 || listSize == (int)Kaifa::List2 || listSize == (int)Kaifa::List3)
	{
		if (listSize == (int)Kaifa::List1)
		{
			if (debugger) debugger->println(" (list #1 has no ID)");
		}
		else
		{
			String id = hanReader.getString((int)Kaifa_List2::ListVersionIdentifier);
			if (debugger) debugger->println(id);
		}

		// Get the timestamp (as unix time) from the package
		time_t time = hanReader.getPackageTime();
		if (debugger) debugger->print("Time of the package is: ");
		if (debugger) debugger->println(time);

		// Define a json object to keep the data
		//StaticJsonBuffer<500> jsonBuffer;
		DynamicJsonBuffer jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();

		// Any generic useful info here
		root["id"] = WiFi.macAddress();
		root["up"] = millis();
		root["t"] = time;

		// Add a sub-structure to the json object, 
		// to keep the data from the meter itself
		JsonObject& data = root.createNestedObject("data");

		// Get the temperature too
		tempSensor.requestTemperatures();
		float temperature = tempSensor.getTempCByIndex(0);
		data["temp"] = String(temperature);

		// Based on the list number, get all details 
		// according to OBIS specifications for the meter
		if (listSize == (int)Kaifa::List1)
		{
			data["P"] = hanReader.getInt((int)Kaifa_List1::ActivePowerImported);
		}
		else if (listSize == (int)Kaifa::List2)
		{
			data["lv"] = hanReader.getString((int)Kaifa_List2::ListVersionIdentifier);
			data["id"] = hanReader.getString((int)Kaifa_List2::MeterID);
			data["type"] = hanReader.getString((int)Kaifa_List2::MeterType);
			data["P"] = hanReader.getInt((int)Kaifa_List2::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kaifa_List2::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kaifa_List2::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kaifa_List2::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kaifa_List2::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kaifa_List2::VoltageL1);
			data["U2"] = hanReader.getInt((int)Kaifa_List2::VoltageL2);
			data["U3"] = hanReader.getInt((int)Kaifa_List2::VoltageL3);
		}
		else if (listSize == (int)Kaifa::List3)
		{
			data["lv"] = hanReader.getString((int)Kaifa_List3::ListVersionIdentifier);;
			data["id"] = hanReader.getString((int)Kaifa_List3::MeterID);
			data["type"] = hanReader.getString((int)Kaifa_List3::MeterType);
			data["P"] = hanReader.getInt((int)Kaifa_List3::ActiveImportPower);
			data["Q"] = hanReader.getInt((int)Kaifa_List3::ReactiveImportPower);
			data["I1"] = hanReader.getInt((int)Kaifa_List3::CurrentL1);
			data["I2"] = hanReader.getInt((int)Kaifa_List3::CurrentL2);
			data["I3"] = hanReader.getInt((int)Kaifa_List3::CurrentL3);
			data["U1"] = hanReader.getInt((int)Kaifa_List3::VoltageL1);
			data["U2"] = hanReader.getInt((int)Kaifa_List3::VoltageL2);
			data["U3"] = hanReader.getInt((int)Kaifa_List3::VoltageL3);
			data["tPI"] = hanReader.getInt((int)Kaifa_List3::CumulativeActiveImportEnergy);
			data["tPO"] = hanReader.getInt((int)Kaifa_List3::CumulativeActiveExportEnergy);
			data["tQI"] = hanReader.getInt((int)Kaifa_List3::CumulativeReactiveImportEnergy);
			data["tQO"] = hanReader.getInt((int)Kaifa_List3::CumulativeReactiveExportEnergy);
		}

		// Write the json to the debug port
		if (debugger) {
			debugger->print("Sending data to MQTT: ");
			root.printTo(*debugger);
			debugger->println();
		}

		// Make sure we have configured a publish topic
		if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
			return;

		// Publish the json to the MQTT server
		char msg[1024];
		root.printTo(msg, 1024);
		mqtt.publish(ap.config.mqttPublishTopic, msg);
	}
}

// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void MQTT_connect() 
{
	// Connect to WiFi access point.
	if (debugger)
	{
		debugger->println(); 
		debugger->println();
		debugger->print("Connecting to WiFi network ");
		debugger->println(ap.config.ssid);
	}

	if (WiFi.status() != WL_CONNECTED)
	{
		// Make one first attempt at connect, this seems to considerably speed up the first connection
		WiFi.disconnect();
		WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
		delay(1000);
	}

	// Wait for the WiFi connection to complete
	long vTimeout = millis() + WIFI_CONNECTION_TIMEOUT;
	while (WiFi.status() != WL_CONNECTED) {
		delay(50);
		if (debugger) debugger->print(".");
		
		// If we timed out, disconnect and try again
		if (vTimeout < millis())
		{
			if (debugger)
			{
				debugger->print("Timout during connect. WiFi status is: ");
				debugger->println(WiFi.status());
			}
			WiFi.disconnect();
			WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
			vTimeout = millis() + WIFI_CONNECTION_TIMEOUT;
		}
		yield();
	}

	if (debugger) {
		debugger->println();
		debugger->println("WiFi connected");
		debugger->println("IP address: ");
		debugger->println(WiFi.localIP());
		debugger->print("\nconnecting to MQTT: ");
		debugger->print(ap.config.mqtt);
		debugger->print(", port: ");
		debugger->print(ap.config.mqttPort);
		debugger->println();
	}

	// Wait for the MQTT connection to complete
	while (!mqtt.connected()) {
		
		// Connect to a unsecure or secure MQTT server
		if ((ap.config.mqttUser == 0 && mqtt.connect(ap.config.mqttClientID)) || 
			(ap.config.mqttUser != 0 && mqtt.connect(ap.config.mqttClientID, ap.config.mqttUser, ap.config.mqttPass)))
		{
			if (debugger) debugger->println("\nSuccessfully connected to MQTT!");

			// Subscribe to the chosen MQTT topic, if set in configuration
			if (ap.config.mqttSubscribeTopic != 0 && strlen(ap.config.mqttSubscribeTopic) > 0)
			{
				mqtt.subscribe(ap.config.mqttSubscribeTopic);
				if (debugger) debugger->printf("  Subscribing to [%s]\r\n", ap.config.mqttSubscribeTopic);
			}
		}
		else
		{
			if (debugger)
			{
				debugger->print(".");
				debugger->print("failed, mqtt.state() = ");
				debugger->print(mqtt.state());
				debugger->println(" trying again in 5 seconds");
			}

			// Wait 2 seconds before retrying
			mqtt.disconnect();
			delay(2000);
		}

		// Allow some resources for the WiFi connection
		yield();
	}
}

// Send a simple string embedded in json over MQTT
void sendMqttData(String data)
{
	// Make sure we have configured a publish topic
	if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
		return;

	// Make sure we're connected
	if (!client->connected() || !mqtt.connected()) {
		MQTT_connect();
	}

	// Build a json with the message in a "data" attribute
	DynamicJsonBuffer jsonBuffer;
	JsonObject& json = jsonBuffer.createObject();
	json["id"] = WiFi.macAddress();
	json["up"] = millis();
	json["data"] = data;

	// Stringify the json
	String msg;
	json.printTo(msg);

	// Send the json over MQTT
	mqtt.publish(ap.config.mqttPublishTopic, msg.c_str());
}