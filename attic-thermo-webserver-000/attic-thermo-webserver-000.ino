#define __MAIN_INO__
unsigned long int stored_temperatures = 0;
#define USE_DALLAS      // Using DS18B20?
// #define DUMP_DATA_SERIAL  // dump temp data, on hits, to serial
//#define USE_DHT11       // Using DHT11 (d1 mini shield in my case)
#define SAVE_SPIFFS     // save temperature setting to spiffs

#ifdef SAVE_SPIFFS
//#define INIT_SPIFFS   // define if you want to format fs
#endif

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
//#include <ESP8266mDNS.h> 
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#ifdef SAVE_SPIFFS
//#include "FS.h"
#include "LittleFS.h" // LittleFS is declared
#endif
#include "ota.h"

#ifdef USE_DALLAS
#include <OneWire.h>
#include <DallasTemperature.h>
#define DALPIN 0   // data pin for ds18b20
OneWire oneWire(DALPIN); // on our pin (bus), but for any onewire devices
DallasTemperature ds18sensors(&oneWire); // Pass our oneWire ref to Dallas Temperature. 
#endif

//#include <ArduinoOTA.h>
#include "wifi_config.h" // This sets actual values (gross): ssid, password. ip, gw, nm

#define RELAYPIN 5  // D1 = 1 for NodeMCU, but we're using Arduino so D1 = 5

#ifdef USE_DHT
#include "DHT.h"
#define DHTPIN  2     // D4 = 2 (D4, 2, is shield's pin)
#define DHTTYPE DHT11 // DHT11
#endif

#define VERBOSE 2

//#include "time.c" // including a .c, yay!  ssid, password.  ip, gw, nm

#define sp(a) Serial.print(a)
#define sl(a) Serial.println(a)

#define TYPE_DEGF 1  // Graph types: Degrees / humidity
#define TYPE_HUM  2
#define GET_TEMP_ATTEMPTS 3 // attempts to make reading therm
#define ROOT_MAX_HTML 800    // size of HTML buffer
#define SMALL_HTML    300    // size of HTML buffer

#define DELAY_ERROR 500
#define DELAY_NORMAL 500

unsigned long previous_millis=0;
unsigned long secs_counter=0;
unsigned long time_last = 0;
unsigned char fan_on_manual = 0;
unsigned long fan_on_limit_secs = 0;
ESP8266WebServer server(80); //main web server
ESP8266HTTPUpdateServer httpUpdater;
const int led = 13;
#ifdef USE_DHT
DHT dht(DHTPIN, DHTTYPE);
#endif

struct temphum_data_minimal {
	#ifdef USE_DHT11
		float h, f;
	#endif
	#ifdef USE_DALLAS
		float df;
	#endif
};
struct temphum_data_detailed {
	#ifdef USE_DHT11
		float h, c, f, hic, hif;
		char hs[7], cs[7], fs[7], hics[7], hifs[7];
	#endif
	#ifdef USE_DALLAS
		float df; // for dallas reading degf
	#endif
};
#define DEF_FAN_TEMP  110 // some days it's even hotter than this always though
#define FN_FANTEMP "/f.txt"
#define FAN_MIN_SECS 60
#define FAN_THRESH 1    // turn off after fanTemp-this_value

/* SECS_TEMP_CHECK
   How frequently to check temperature, just for display.
   Updates to data handle less frequently.
   (See CHECK_PERIOD for the seconds between storage points)
*/
#define SECS_TEMP_CHECK 5

#define MAX_DATAPOINTS 1440
//#define CHECK_PERIOD 3  // seconds
//#define CHECK_PERIOD 120  // seconds
//#define CHECK_PERIOD 5  // seconds
#define CHECK_PERIOD 10  // seconds
//#define DAY_DATAPOINTS (48*60*60/CHECK_PERIOD) // Every minute
#define DAY_DATAPOINTS MAX_DATAPOINTS
//#define DAY_DATAPOINTS (24*15)
#define WEB_REFRESH_SECS "120"
#define WEB_REFRESH_SECSI 120

int lastFanChange = 0;
int relaystate=LOW;
int fanOnTemp=DEF_FAN_TEMP;
char *lastError="";
// longterm data: Add 1 just in case we're dumb and overrun
struct temphum_data_minimal dayData[DAY_DATAPOINTS+1];
int dayStart=0; // unused
int dayNext=0;
#define TEMPREAD_DELAY_MILLIS 1000  // millis
unsigned long last_tempread_millis=0;
#define TEMP_HF_CNT 60
#define TEMP_HF_MEDIAN_CNT 15
#if TEMP_HF_MEDIAN_CNT > TEMP_HF_CNT
	#error "TEMP_HF_MEDIAN_CNT too big. Must be smaller buffer than TEMP_HF_CNT"
#endif
struct temphum_data_detailed recent_hf_temps[TEMP_HF_CNT];
struct temphum_data_detailed cprectemps[TEMP_HF_MEDIAN_CNT];
#ifdef USE_DALLAS
//uint8_t dal_addr=0; // this address method might be faster than byindex
#endif

FSInfo fs_info;

void drawGraph(int type);
void drawGraphF(void);
void drawGraphH(void);

void reset_lasttime(void) {
	time_last = millis()/1000;
}

// Get from Circle Buffer: agei is age offset, a positive number
//                         (1 would be 1 sample ago)
//                         0 is the last-read sample
struct temphum_data_minimal *get_lf_aged_samplep(int agei) {
	// Macro version: ((dat)[(nxtv)-(n)>0 ? (nxtv)-(n)-1 : (tot)-(n)-1])
	int offset = dayNext - 1 - agei;
	if (offset < 0) offset += DAY_DATAPOINTS;
	return dayData + offset;
}

// Get minimum and maximum temps
// type: TYPE_DEGF or TYPE_HUM
// if no dht11, TYPE_HUM sets min=0 max=1
void get_day_minmax(float *minf, float *maxf, int type) {
	*minf = 400;  // start high, and low
	*maxf = -400;
	for (int i=0; i<DAY_DATAPOINTS; i++) {
		if (type == TYPE_DEGF) {
			#ifdef USE_DHT11
				float valdht;
				valdht = dayData[i].f;
				if (valdht > *maxf) *maxf = valdht;
				if (valdht < *minf) *minf = valdht;
			#endif

			#ifdef USE_DALLAS
				float valds;
				valds = dayData[i].df;
				if (valds > *maxf) *maxf = valds;
				if (valds < *minf) *minf = valds;
			#endif
		} else {
			#ifdef USE_DHT11
				float val;
				val = dayData[i].h; // type probably TYPE_HUM
				if (val > *maxf) *maxf = val;
				if (val < *minf) *minf = val;
			#else
			*minf = 0;          // if no dht11, just use a 0-1 range
			*maxf = 1;
			#endif
		}
	}
	if (*minf == *maxf) *minf = *maxf-1;
	//if (*minf == 0.0) *minf = *maxf - 1;
}

// humidity, degc, degf, heat index c, heat index f
// ** does not fill string values in struct
int get_temphum_floats(struct temphum_data_detailed *tdp) {
	#ifdef USE_DALLAS
		float res;
		ds18sensors.requestTemperatures();
		res = ds18sensors.getTempFByIndex(0);
		/* char temp[SMALL_HTML]; */
		/* snprintf(temp, SMALL_HTML, "DS18 DegF: %.8f floor=>%.8f int=>%d", res, floor(res), (int)res); */
		/* sl(temp); */
		// Disconnected DS18B20 data line yields DEVICE_DISCONNECTED_F
		// Disc. power lead yields 185 (also from getTempFbyindex())
		if ((int)res == -196 || (int)res == 185) {
			sl(lastError);
			lastError = "DS18B20[0] !connected";
			return 1;
		}
		lastError = "No error";
		tdp->df = res;
	#endif
	#ifdef USE_DHT11
		tdp->h = dht.readHumidity(1); // force read
		tdp->c = dht.readTemperature(false, 1); // celsius, (force it)
		tdp->f = dht.readTemperature(true, 1);  // fahrenheit, (force it)
		if (isnan(tdp->h)) { // extra tests just for more verbose reporting
			Serial.print("Failed hum read\n");
		}
		if (isnan(tdp->c) || isnan(tdp->f)) {
			Serial.print("Failed temp read\n");
		}
		if (isnan(tdp->h) || isnan(tdp->c) || isnan(tdp->f)) {
			//Serial.print("Failed to read hum, degc, or degf\n");
			return 1;
		}
		tdp->hic = dht.computeHeatIndex(tdp->c, tdp->h, false);       
		tdp->hif = dht.computeHeatIndex(tdp->f, tdp->h);       
		// You can delete the following Serial.print's, it's just for debugging purposes
		Serial.print("Hum: ");
		Serial.print(tdp->h);
		Serial.print(" %\t Temp: ");
		Serial.print(tdp->c);
		Serial.print(" *C ");
		Serial.print(tdp->f);
		Serial.print(" *F\t Heat index: ");
		Serial.print(tdp->hic);
		Serial.print(" *C ");
		Serial.print(tdp->hif);
		Serial.print(" *F\n");
	#endif
	return 0;
}

#if 0 // not using strings
int float_to_s7(char *str, float f) {
	dtostrf(f, 6, 2, str); 
}

int get_temphum_all(struct temphum_data_detailed *td) {
	float h, c, f, hic, hif;
	int rc;
	if ((rc = get_temphum_floats(td))) {
    	//sl("Failed to read from DHT sensor!");
	    strcpy(td->hs, "Fail");
	    strcpy(td->cs, "Fail");
	    strcpy(td->fs, "Fail");         
	    strcpy(td->hics, "Fail");         
	    strcpy(td->hifs, "Fail");         
		return 1;
	}
	float_to_s7(td->hs, td->h);
	float_to_s7(td->cs, td->c);
	float_to_s7(td->fs, td->f);
	float_to_s7(td->hics, td->hic);
	float_to_s7(td->hifs, td->hif);
	return 0;
}
#endif

void load_firstreading(void) {
	struct temphum_data_detailed td;
	int succ=0;
	// try a lot of times to get a reading
	sl("Reading initial temp data");

	for (int i=0; i<GET_TEMP_ATTEMPTS; i++) {
		if (!get_temphum_floats(&td)) {
			succ++;
		} else {
			sl("Failed initial read, trying 3 times\n");
		}
		delay(100);
	}
	if (!succ) {
		#ifdef USE_DHT11
			td.h = 0;
			td.f = 0;
		#endif
		#ifdef USE_DALLAS
			td.df = 0;
		#endif
	}
	// Whether failed reads or not, we still set all the storage
	// values -- at least they'll be consistent.
	for (int i=0; i<DAY_DATAPOINTS; i++) {
		#ifdef USE_DHT11
			dayData[i].h = td.h;
			dayData[i].f = td.f;
		#endif
		#ifdef USE_DALLAS
			dayData[i].df = td.df;
			/* dayData[i].df = 1.0; */
		#endif
	}
	for (int i=0; i<TEMP_HF_CNT; i++) {
		recent_hf_temps[i] = td;
	}
}

#if 0 // working on this
void refresh_printf(int secs, char *url, char *htmlopt, ...) {
	char temp[SMALL_HTML];
	int len;
	len = snprintf(temp, SMALL_HTML,
		"<html><head><meta http-equiv=refresh content='%d; url=%s' /></head><body>",
		secs, url);
	if (len < SMALL_HTML) { // enough room left
		snprintf(temp+len, htmlopt 
			, htmlopt ? htmlopt : ""
		);
	}
	server.send(200, "text/html", temp );
}
#endif

void refresh_send(int secs, const char *url, const char *htmlopt) {
	char temp[SMALL_HTML];
	snprintf(temp, SMALL_HTML,
		"<html><head><meta http-equiv=refresh content='%d; url=%s' /></head><body>%s</body></html>",
		secs, url, htmlopt ? htmlopt : ""
	);
	server.send(200, "text/html", temp);
}

void fanOnHTTP() {
	String minss=server.arg("m"); // minutes (optional) (actually seconds)
	unsigned int minsi;

	if (!minss.length() || !isdigit(minss[0])) { // Manual setting
		minsi = 0;
		Serial.print(F("No m setting passed"));
	} else {
		char lastc = minss[minss.length()-1];
		minsi = minss.toInt();
		if (lastc == 'm') minsi *= 60;
		else if (lastc == 'h') minsi *= 60*60;
		fan_on_manual = 1;
		fan_on_limit_secs = minsi;
		Serial.print(F("Setting mins to "));
		Serial.println(minsi);
	}

	fanOn();
	refresh_send(2, "/", (char *)F("Turned on"));
}

void fanOffHTTP() {
	fan_on_manual = 1;
	fan_on_limit_secs = 0;
	fanOff();
	refresh_send(2, "/", (char *)F("Turned off"));
}
void fanOn() {
	digitalWrite(RELAYPIN, relaystate = HIGH);
}
void fanOff() {
	digitalWrite(RELAYPIN, relaystate = LOW);
}

void handleRoot() {
	digitalWrite ( led, 1 );
	char temp[ROOT_MAX_HTML+1];
	int sec = millis() / 1000; // total secs, not mod(60)
	int min = sec / 60;        // none of these are mod. they're totals.
	int hr = min / 60;
	int days = hr / 24;
	struct temphum_data_minimal *tdp;
	float minf, maxf;
	float minh, maxh;
	String out = "";

	relaystate = digitalRead(RELAYPIN);
	sp("Fan relay current state: "); sl(relaystate);

	get_day_minmax(&minf, &maxf, TYPE_DEGF); // handles dallas and dht11
	get_day_minmax(&minh, &maxh, TYPE_HUM);  // invalid results if no dht11
	//int i = dayNext-1>0 ? dayNext-2 : DAY_DATAPOINTS-2;
	#ifdef DEBUG_DUMP_TEMPS_SERIAL
		#ifdef USE_DALLAS
			sp("DS18B20 DegF: ");
			for (int i=0; i<DAY_DATAPOINTS; i+=int(DAY_DATAPOINTS/15)) {
				tdp = get_lf_aged_samplep(i);
				sp(tdp->df); sp(" ");
			}
			sp("\n");
		#endif
		#ifdef USE_DHT11
			sp("DHT11 DegF: ");
			for (int i=0; i<DAY_DATAPOINTS; i+=int(DAY_DATAPOINTS/15)) {
				tdp = get_lf_aged_samplep(i);
				sp(tdp->f); sp(" ");
			}
			sp("\nHum: ");
			for (int i=0; i<DAY_DATAPOINTS; i+=int(DAY_DATAPOINTS/15)) {
				tdp = get_lf_aged_samplep(i);
				sp(tdp->h); sp(" ");
			}
			sp("\n");
		#endif
	#endif
	tdp = get_lf_aged_samplep(0);
	server.sendContent("HTTP/1.0 200 OK\r\n");
	server.sendContent("Content-Type: text/html\r\n\r\n");
	snprintf(temp, ROOT_MAX_HTML,
		"<html>"
		"<head>"
		"<meta http-equiv=refresh content=" WEB_REFRESH_SECS " />"
		"<meta charset='utf-8' />"
		"<title>Attic</title>"
		"<style>"
		"body{background:#eee;font-family:Sans-Serif;color:#008;font-size:170%%;}"
		"form{padding:.1em .5em .1em .5em}"
		"input{font-size:170%%;}"
		"img{background:MidnightBlue}"
		"p{margin:.5em .2em .5em .2em}"
		".f{padding:0em 1em 0em 1em}" // padded data field
		".fl{padding:0em 1em 0em .1em; font-size:65%%}" // padded data field
		".fs{color:white;font-weight:bold}" // fan state
		".on{background:green}"
		".off{background:red}"
		".t{background:yellow}"          // temp
		".tl{background:#ffffbb}"        // multi temp listing
		".sss{font-size:30%%;}"          // extra extra small
		"</style>"
		"</head>"
		"<body>\n");
	server.sendContent(temp);

	snprintf(temp, ROOT_MAX_HTML,
		"<p>Uptime: %d days, %02dh %02dm %02ds [<a href=/update>Update</a>]<br/>"
		"<small>Page reload in %ds</small><br/>"
		"<i>Fan on @</i> %d°, Fan state: %s<br/>"
		"Fan manually set: %s (Time left: %ldh%ldm%lds)<br/>"
		"[<a href=foff>Off</a>]"
		" [<a href=fon>On</a>: "
		"{<a href=fon?m=30>30</a>,<a href=fon?m=60>60</a>}s "
		"{<a href=fon?m=5m>5</a>,<a href=fon?m=20m>20</a>,"
		 "<a href=fon?m=30m>30</a>,<a href=fon?m=45m>45</a>}m "
		"{<a href=fon?m=1h>1</a>,<a href=fon?m=2h>2</a>,"
		 "<a href=fon?m=3h>3</a>,<a href=fon?m=4h>4</a>,"
		 "<a href=fon?m=5h>5</a>}h]<br/>"
		"",
		days, hr%24, min%60, sec%60,
		WEB_REFRESH_SECSI,
		fanOnTemp,
		relaystate == LOW
			? "<span class='f on'>OFF</span>"
			: "<span class='f off'>ON</span>",
		fan_on_manual ? "On" : "Off",
		fan_on_limit_secs/(60*60),
		(fan_on_limit_secs/60)%60,
		fan_on_limit_secs%60
		);
	Serial.println(fan_on_limit_secs);
	server.sendContent(temp);
#ifdef USE_DHT11
	snprintf(temp, ROOT_MAX_HTML,
		"DHT °F: %.2f (min: %.2f, max: %.2f)<br/>"
		"DHT Hum: %.2f, Min: %.2f Max: %.2f<br/>"
		"",
		tdp->f, minf, maxf, tdp->h, minh, maxh);
	out += temp;
#endif
#ifdef USE_DALLAS
	snprintf(temp, ROOT_MAX_HTML,
		"Current temperature: <span class='f t'>%.2f°</span><br/>",
		tdp->df);
	out += temp;
	server.sendContent(out); out = "";

	//////////// Temperature listing

	snprintf(temp, ROOT_MAX_HTML,
		"<div class=sss>All stored temps [start = %d, next = %d] (total stored ever: %lu): ",
		dayStart, dayNext,
		stored_temperatures);
	server.sendContent(temp);
	for (int i=0; i<DAY_DATAPOINTS; i++) {
		tdp = dayData+i;
		if (dayNext == i) {
			snprintf(temp, ROOT_MAX_HTML, "<b>[ %.2f ]</b>, ", tdp->df);
		} else {
			snprintf(temp, ROOT_MAX_HTML, "%.2f, ", tdp->df);
		}
		out = temp;
		server.sendContent(out);
		out="";
	}

	server.sendContent("</div>");
	server.sendContent("<div class=sss>All stored temps in time order: ");
	for (int i=0; i<DAY_DATAPOINTS; i++) {
		tdp = get_lf_aged_samplep(i);
		snprintf(temp, ROOT_MAX_HTML, "%.2f, ", tdp->df);
		out = temp;
		server.sendContent(out);
		out="";
	}
	server.sendContent("</div>");

	sprintf(temp, "Current high-freq temperatures (%d x every %ds):<br/>\n"
			"<span class='fl tl'>",
		TEMP_HF_CNT,
		TEMPREAD_DELAY_MILLIS/1000);
	out += temp;
	for (int i=TEMP_HF_CNT-1; i>=0; i--) {
		sprintf(temp, "%.2f°", recent_hf_temps[i].df);
		out += temp;
		if (i>0) out += ", ";
	}
	out += "</span><br/>";

	//////////// Sorted emperature listing

	median_hf_temp();

	sprintf(temp, "Sorted (most recent %d):<br/>\n"
			"<span class='fl tl'>",
		TEMP_HF_MEDIAN_CNT);
	out += temp;
	for (int i=TEMP_HF_MEDIAN_CNT-1; i>=0; i--) {
		sprintf(temp, "%.2f°", cprectemps[i].df);
		out += temp;
		if (i>0) out += ", ";
	}
	out += "</span><br/>";


#endif
	server.sendContent(out);
	out="";
	snprintf(temp, ROOT_MAX_HTML,
		"XRange %dh%dm%ds. %d total samples, every %ds<br/>"
		"Data storage size: %d<br/>"
		"FS used/total: %d/%d bytes<br/>"
		"Max: %.2f°<br/>"
		//"<img src=/f.svg /><br/>"
		"[ <a href=/f.svg>Graph</a> ]<br/>"
		"Min: %.2f°<br/>"
		"",
		int(DAY_DATAPOINTS * CHECK_PERIOD / 60 / 60),   // all ints anyway
		int((DAY_DATAPOINTS * CHECK_PERIOD / 60)) % 60, // have to be sure with %
		int(DAY_DATAPOINTS * CHECK_PERIOD) % 60,
		DAY_DATAPOINTS,
		CHECK_PERIOD,
		sizeof(dayData),
		fs_info.usedBytes, fs_info.totalBytes,
		maxf, minf
		);
	out += temp;
#ifdef USE_DHT11
	out += F("<img src=/h.svg />");
#endif
	snprintf(temp, ROOT_MAX_HTML,
		"<form action=sett><input type=number name=n value=%d size=4><input type=submit value=Set></form>"
		"Choose: ["
		"<a href='sett?n=70'>70</a>, "
		"<a href='sett?n=75'>75</a>, "
		"<a href='sett?n=80'>80</a>, "
		"<a href='sett?n=115'>115</a>, "
		"<a href='sett?n=120'>120</a>, "
		"<a href='sett?n=125'>125</a>"
		"] °F"
		"</body></html>"
		"",
		fanOnTemp);
	out += temp;
	server.sendContent(out);
	digitalWrite(led, 0);
}

void handleNotFound() {
	char temp[SMALL_HTML];
	digitalWrite ( led, 1 );
	snprintf(temp, SMALL_HTML,
		"File Not Found\n\n"
		"URI: %s\nMethod: %s\nArguments: %d\n"
		"",
		server.uri().c_str(),
		( server.method() == HTTP_GET ) ? "GET" : "POST",
		server.args());
	String msg = temp;
	for ( uint8_t i = 0; i < server.args(); i++ ) {
		msg += " " + server.argName ( i ) + ": " + server.arg ( i ) + "\n";
	}

	server.send ( 404, "text/plain", msg );
	digitalWrite ( led, 0 );
}

void loadTempTrigger(void) {
	LittleFS.begin();
	File f = LittleFS.open(FN_FANTEMP, "r");
	if (!f) sl(F("open failed")), fanOnTemp=DEF_FAN_TEMP;
	else {
		fanOnTemp = f.parseInt();
		sp(F("Loaded temp: "));
		sl(fanOnTemp);
		f.close();
	}
	LittleFS.end();
}

void setTempTrigger(void) {
	String ns=server.arg("n");
	//char temp[110];
	if (!isdigit(ns[0])) refresh_send(5, "/", "BAD NUMBER!");
	else {
		LittleFS.begin();
		File f = LittleFS.open(FN_FANTEMP, "w");
		if (!f) sl("open error");
		else {
			fanOnTemp = ns.toInt();
			f.print(fanOnTemp);
			f.close();
			refresh_send(2, "/", "Set temp");
			sp("set temp to ");
			sl(fanOnTemp);
		}
		LittleFS.end();
	}
}

void setup(void) {
	// We set the initial millis() at the end of setup()
	// >> previous_millis = millis();
	Serial.begin(115200);
#ifdef INIT_SPIFFS
	delay(3000);
	Serial.println(F("Wait 30 secs LittleFS format"));
	bool result = LittleFS.format();
	Serial.print(F("LittleFS formatted result:"));
	Serial.println(result);
#else
  Serial.print(F("Skipping LittleFS format")); 
#endif

#ifdef USE_DHT11
	dht.begin();
#endif
#ifdef USE_DALLAS
	ds18sensors.begin();
#endif

	relaystate = digitalRead(RELAYPIN);
	sp(F("Fan relay current state: ")); sl(relaystate);
	pinMode(RELAYPIN, OUTPUT);
	pinMode(led, OUTPUT);
	digitalWrite(led, 0);
	fanOff();
	WiFi.mode(WIFI_STA);
	WiFi.config(ip, gw, nm);
	WiFi.begin(ssid, password);
	sl("");

	// Wait for connection
	/*
	while ( WiFi.status() != WL_CONNECTED ) {
		delay(500);
		sl("WiFi.");
	} */
	while (WiFi.waitForConnectResult() != WL_CONNECTED) {
		sl(F("Conn. fail! Rebooting..."));
		delay(5000);
		ESP.restart();
	}

	Serial.print ( F("Connected->") );
	Serial.println ( ssid );
	Serial.print ( F("IP: ") );
	Serial.println ( WiFi.localIP() );


	//if ( MDNS.begin ( "esp8266" ) ) { Serial.println ( "MDNS responder started" ); }

	load_firstreading();
	reset_lasttime();

	loadTempTrigger();

	server.on(F("/"), handleRoot );
	server.on(F("/fon"), fanOnHTTP );
	server.on(F("/foff"), fanOffHTTP );
	server.on(F("/f.svg"), drawGraphF );
	server.on(F("/h.svg"), drawGraphH );
	server.on(F("/f.txt"), dumpDataF );
	server.on(F("/sett"), setTempTrigger );
	//server.on("/inline", []() {server.send(200,"text/plain","this works as well"); });
	httpUpdater.setup(&server, update_user, update_pw); // adds /update path for OTA
	server.onNotFound ( handleNotFound );
	server.begin();
	Serial.println ( F("HTTP svr started") );

#ifdef SAVE_SPIFFS
	//LittleFS.begin();
#endif
	LittleFS.begin();
	LittleFS.info(fs_info);
	Serial.print(F("LittleFS total bytes: "));
	Serial.println(fs_info.totalBytes);
	LittleFS.end();
	setup_ota();
	previous_millis = millis();
}

// Gets temperature, puts into struct at tdp.
// Then stores log values to dayData
// Increments to next sensor storage location
// Does NOT increment if read error from sensor(s)
int get_temp(struct temphum_data_detailed *tdp) {
	int rc=1;
	for (int i=0; i<GET_TEMP_ATTEMPTS; i++) {
		if (!(rc = get_temphum_floats(tdp))) break;
		delay(100);
	}
	if (rc) {
		sl(F("Sens failure"));
		return rc;  // failed
	}
	return 0;
}

void store_temp(struct temphum_data_detailed *tdp) {
	struct temphum_data_minimal *storep;
	stored_temperatures++;
	storep = dayData + dayNext;
	#ifdef USE_DHT11
		storep->h = tdp->h;
		storep->f = tdp->f;
	#endif
	#ifdef USE_DALLAS
		storep->df = tdp->df;
	#endif
	dayNext++;
	if (dayNext >= DAY_DATAPOINTS) dayNext=0;
	//return 0;
}

#if 0 // no more since we work from median of higher frequency samples
int get_and_store_temp(struct temphum_data_detailed *tdp) {
	int rc=1;
	struct temphum_data_minimal *storep;
	for (int i=0; i<GET_TEMP_ATTEMPTS; i++) {
		if (!(rc = get_temphum_floats(tdp))) break;
		delay(100);
	}
	if (rc) {
		sl(F("Sens failure"));
		return rc;  // failed
	}
	storep = &(dayData[dayNext]);
	#ifdef USE_DHT11
		storep->h = tdp->h;
		storep->f = tdp->f;
	#endif
	#ifdef USE_DALLAS
		storep->df = tdp->df;
	#endif
	if (++dayNext >= DAY_DATAPOINTS) dayNext=0;
	return 0;
}
#endif

void manual_fan_update(unsigned long diff_secs) {
	//unsigned int diff_mins = diff_secs/60;
	if (fan_on_manual && relaystate == HIGH) {
		if (fan_on_limit_secs < diff_secs) {
			fan_on_limit_secs = 0;
			fan_on_manual = 0;
			fanOff();
		} else {
			fan_on_limit_secs -= diff_secs;
		}
	}
}

int sort_det_temps_asc(const void *d1, const void *d2) {
	struct temphum_data_detailed *r1=(struct temphum_data_detailed *)d1;
	struct temphum_data_detailed *r2=(struct temphum_data_detailed *)d2;
	if (r1->df < r2->df) return 1;
	if (r1->df > r2->df) return -1;
	return 0;
}

struct temphum_data_detailed median_hf_temp() {
	memcpy(cprectemps,
	       recent_hf_temps,
	       TEMP_HF_MEDIAN_CNT * sizeof(*recent_hf_temps));
	qsort(cprectemps, sizeof cprectemps, sizeof *cprectemps, sort_det_temps_asc);
	return cprectemps[ (int)(TEMP_HF_MEDIAN_CNT / 2) ];
}

int update_hf_temp() {
	struct temphum_data_detailed reading;
	if (!get_temp(&reading)) { // success
		for (int i=TEMP_HF_CNT-2; i>=0; i--) {
			recent_hf_temps[i+1] = recent_hf_temps[i]; // shift up
		}
		recent_hf_temps[0] = reading; // copy struct
		return 0;
	}
	return 1;
}

void temphumLoopHandler(void) {
	unsigned long secsNow;
	int seconds;
	struct temphum_data_detailed td;

	unsigned long cur_millis = millis();

	unsigned long diff_millis = cur_millis - previous_millis;
	unsigned long diff_secs;
	if (diff_millis >= 1000) { // at least a second has passed.
		diff_secs = diff_millis / 1000;
		secs_counter += diff_secs;
		previous_millis += (diff_secs * 1000);
		Serial.print(F("Diff millis: ")); Serial.println(diff_millis);
		Serial.print(F("Diff secs: ")); Serial.println(diff_secs);
		Serial.print(F(" Millis: ")); Serial.println(millis());
		Serial.print(F("PMillis: ")); Serial.println(previous_millis);
		manual_fan_update(diff_secs);
	}

	secsNow = cur_millis / 1000;	// the number of milliseconds that have passed since boot
	seconds = secsNow - time_last;	//the number of seconds that have passed since the last time CHECK_PERIOD seconds was reached.
	if (cur_millis - last_tempread_millis > TEMPREAD_DELAY_MILLIS) {
		// Re-read temperature
		if (!update_hf_temp()) { // success
			last_tempread_millis = cur_millis;
		}
	}

	if (seconds >= CHECK_PERIOD) {   // at or past time to get temperature
		time_last = secsNow;  // Reset timer even if we don't get the data
			// Try twice to get temp data
		td = median_hf_temp();
		store_temp(&td);
		if (1) {
			float curf;
			#ifdef USE_DALLAS // more reliable
				curf = td.df;
			#elif defined(USE_DHT11)
				curf = td.f;
			#else
				#error "NO THERMOMETER!"
			#endif
			
			// Test if fan needs to be turned on or off
			// Don't do it unless FAN_MIN_SECS time has passed, to avoid
			// it turning on/off too quickly.
			if (!fan_on_manual) {
				if (secsNow - lastFanChange > FAN_MIN_SECS) {
					if (curf >= fanOnTemp && relaystate == LOW) {
						sl(F("Fan turned ON"));
						lastFanChange = secsNow;
						fanOn();
					} else if (curf < fanOnTemp-FAN_THRESH && relaystate == HIGH) {
						sl(F("Fan turned OFF"));
						lastFanChange = secsNow;
						fanOff();
					}
				}
			}
		}
	}
}

#define XSTR(s) STR(s)
#define STR(s) #s

void dumpDataF() {
	String times=server.arg("t");
	uint32_t timei;
	int i, n;
	uint32_t offt=-((DAY_DATAPOINTS-1)*CHECK_PERIOD);
	if (!times.length() || !isdigit(times[0])) {
		timei=0;
	} else {
		timei = times.toInt();
	}
	server.sendContent("HTTP/1.0 200 OK\r\n");
	server.sendContent("Content-Type: text/plain\r\n\r\n");

	server.sendContent("# Datapoints: " XSTR(DAY_DATAPOINTS) ", Sample secs: " XSTR(CHECK_PERIOD) "\n");
	if (timei != 0) {
		server.sendContent("#Time(s)\tTempF\n");
	} else {
		server.sendContent("#Seconds\tTempF\n");
	}

	n=0;
	i=dayNext+1;
	for (;  n<DAY_DATAPOINTS; offt += CHECK_PERIOD, n++, i++) {
		struct temphum_data_minimal *ts;
		char temp[24];
		if (i>DAY_DATAPOINTS) i=0;
		ts = &dayData[i];
		sprintf(temp, "%u\t%3.3f\n", timei+offt, ts->df);
		server.sendContent(temp);
	}
}

void drawGraphF() { drawGraph(TYPE_DEGF); }
void drawGraphH() {
#ifdef USE_DHT11
	drawGraph(TYPE_HUM);
#else
	sp(F("ERROR: drawGraphH called when DHT11 is not enabled"));
	server.send ( 200, F("image/svg+xml"), "");
#endif
}

void drawGraph(int type) {
	String out = "";
	char temp[SMALL_HTML+1];
	int i, n;
	// Display about 16 datapoints starting at 0 in time (in the circle buffer)
#if defined(USE_DHT11) && defined(DUMP_DATA_SERIAL)
	sp("GRAPH DHT: ");
	n=0, i=dayNext+1;
	for (; n<DAY_DATAPOINTS; n+=(DAY_DATAPOINTS/16), i+=(DAY_DATAPOINTS/16)) {
		struct temphum_data_minimal *ts;
		if (i>DAY_DATAPOINTS) i=0;
		ts = &dayData[i];
		sp(ts->f);
		sp(" ");
	}
	sl("");
#endif
#if defined(USE_DALLAS) && defined(DUMP_DATA_SERIAL)
	sp("GRAPH DS18: ");
	n=0, i=dayNext+1;
	for (; n<DAY_DATAPOINTS; n+=(DAY_DATAPOINTS/16), i+=(DAY_DATAPOINTS/16)) {
		struct temphum_data_minimal *ts;
		if (i>DAY_DATAPOINTS) i=0;
		ts = &dayData[i];
		sp(ts->df);
		sp(" ");
	}
	sl("");
#endif
#define WIDTH  640
#define HEIGHT 280
#define PAD    10
#define PAD2   (PAD*2)
	//void ESP8266WebServer::_prepareHeader(String& response, int code, const char* content_type, size_t contentLength) 

	// Get minimum and maximum temps
	float minv, maxv;
	get_day_minmax(&minv, &maxv, type);
	//sp("Min max: "); sp(minv); sp(" -> "); sl(maxv);
	minv=floor(minv);
	maxv=ceil(maxv);

	String response = "";
	//server._prepareHeader(response, 200, "image/svg+xml", CONTENT_LENGTH_UNKNOWN);
	server.sendContent("HTTP/1.0 200 OK\r\n");
	server.sendContent("Content-Type: image/svg+xml\r\n\r\n");
	//server.setContentLength(CONTENT_LENGTH_UNKNOWN);
	//server.send ( 200, "image/svg+xml", "");
	sprintf(temp,
		"<svg xmlns='http://www.w3.org/2000/svg' version='1.1' width='%d' height='%d'>"
		"",
			WIDTH+PAD2, HEIGHT+PAD2);
	server.sendContent(temp);
	server.sendContent(
		F("<defs>"
		"<style type='text/css'><![CDATA["
		"line{stroke-width:2;}"
		".hg{stroke-width:1;stroke:gray;}"
		".vg{stroke-width:1;stroke:#315A5C;}"
    ".tx{fill:red;font-size:200%;}"
		"]]></style>"
		"</defs>"));
 	//out += temp;

#define RANGECONV(i, smin, smax, dmin, dmax) \
		(((dmax)-(dmin)) * ((i)-(smin)) / ((smax)-(smin)) + (dmin))

	// HORIZONTAL (degrees) GRAPH LINES
	out = F("<g class='hg'>\n");
	for (i=minv; i<=maxv; i++) {
		int y;
		y = RANGECONV(i, (int)minv, (int)maxv, 0, HEIGHT-1);
#if 0 // use if you want to see your range conversions
		snprintf(temp, SMALL_HTML, "%d range [%f,%f] -> [0,%d] becomes %d",
			i, minv, maxv, HEIGHT+PAD2, y);
		sl(temp);
		snprintf(temp, SMALL_HTML, "  Line: %d,%d -> %d,%d",
			0, HEIGHT+PAD2-y,   WIDTH+PAD2-1, HEIGHT+PAD2-y);
		sl(temp);
#endif
 		sprintf(temp, "<line x1='%d' y1='%d' x2='%d' y2='%d' />\n",
			0, HEIGHT+PAD2-y, WIDTH+PAD2-1, HEIGHT+PAD2-y);
		out += temp;
	}
	out += "</g>\n";
	server.sendContent(out);
	out = "";

	//
	// VERTICAL GRID LINES (EVERY HOUR)
	//
	struct temphum_data_minimal *td1, *td2;
	// go from: dayNext -> DAY_DATAPOINTS-1
	//    then: 0 to dayNext-1
	//if (i<0) i=DAY_DATAPOINTS-1;
	// ((360 * 120 = total secs) / 60 / 60 = total hours) = 12
	int xlines = (DAY_DATAPOINTS * CHECK_PERIOD)/60/60;
	out += "<g class='vg'>";
	for (i=0; i<=xlines; i++) {
		int xloc = WIDTH * i / xlines;
	 	sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" />\n",
			xloc+PAD, 0, xloc+PAD, HEIGHT+PAD2);
		out += temp;
	}
	out += "</g>";
	server.sendContent(out);
	out = "";

#ifdef USE_DALLAS
	i=dayNext;
	if (type == TYPE_DEGF) { // only degF graphs for DALLAS
		out = "<g stroke='MediumOrchid'>\n";
		for (n=0; n<DAY_DATAPOINTS-1; n++, i++) {
			int i2;           // i+1's next datapoint index
			int y, y2, x, x2; // output svg coords
			float val1, val2; // humidity or degf (based on type parameter)
	
			if (i>=DAY_DATAPOINTS) i=0; // Handle circular buffer wrap
			i2 = i+1;
			if (i2>=DAY_DATAPOINTS) i2=0;
			td1 = &dayData[i];          // Get structs for data points
			td2 = &dayData[i2];
			val1 = type == TYPE_DEGF ? td1->df : 0;
			val2 = type == TYPE_DEGF ? td2->df : 0;
	
			// Scale X to SVG dimensions
			x = (int)(n * WIDTH * 1.0 / (DAY_DATAPOINTS-1)); // float and back
			x2 = (int)((n+1) * WIDTH * 1.0 / (DAY_DATAPOINTS-1));
			// Scale temp (Y) to SVG dimensions
			y = ((val1 - minv) / (maxv-minv)) * HEIGHT;
			y2 = ((val2 - minv) / (maxv-minv)) * HEIGHT;
	
	 		sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" />\n", x+10, 10 + (HEIGHT-y), x2+10, 10 + (HEIGHT-y2));

			// Send out data since it can get too big
			// HTTP_UPLOAD_BUFLEN comes from ESP8266WebServer.h
			if (out.length()+strlen(temp) >= HTTP_UPLOAD_BUFLEN-1) {
				server.sendContent(out);
				out = "";
			}
	 		out += temp;
		}
		//refresh_send(5, "/", "refresh"); return;
		out += "</g>";
		server.sendContent(out);
	}
#endif
#ifdef USE_DHT11
	out = "<g stroke='red'>\n";
	//sp("Line: "); sl(__LINE__);
	i=dayNext;
	for (n=0; n<DAY_DATAPOINTS-1; n++, i++) {
		int i2;           // i+1's next datapoint index
		int y, y2, x, x2; // output svg coords
		float val1, val2; // humidity or degf (based on type parameter)

		if (i>=DAY_DATAPOINTS) i=0; // Handle circular buffer wrap
		i2 = i+1;
		if (i2>=DAY_DATAPOINTS) i2=0;
		td1 = &dayData[i];          // Get structs for data points
		td2 = &dayData[i2];
		val1 = type == TYPE_DEGF ? td1->f : td1->h;
		val2 = type == TYPE_DEGF ? td2->f : td2->h;

		// Scale X to SVG dimensions
		x = (int)(n * WIDTH * 1.0 / (DAY_DATAPOINTS-1)); // float and back
		x2 = (int)((n+1) * WIDTH * 1.0 / (DAY_DATAPOINTS-1));
		// Scale temp (Y) to SVG dimensions
		y = ((val1 - minv) / (maxv-minv)) * HEIGHT;
		y2 = ((val2 - minv) / (maxv-minv)) * HEIGHT;

 		sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" />\n", x+10, 10 + (HEIGHT-y), x2+10, 10 + (HEIGHT-y2));
 		out += temp;
	}
	//refresh_send(5, "/", "refresh"); return;
	out += "</g>";
	server.sendContent(out);
#endif

	// Random graph
 	/* out += "<g stroke=\"black\">\n";
 	int y = rand() % 130;
 	for (int x = 10; x < 390; x+= 10) {
 		int y2 = rand() % 130;
 		sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\n", x, 140 - y, x + 10, 140 - y2);
 		out += temp;
 		y = y2;
 	}
 	out += "</g>";
	*/
	sprintf(temp,
		"<text x=\"10\" y=\"30\" class=\"tx\">%.1f</text>"
		"<text x=\"10\" y=\"%d\" class=\"tx\">%.1f</text>"
		"\n</svg>\n",
			maxv,
			HEIGHT+PAD2-15,
			minv);
	//out = "</svg>\n";
	server.sendContent(temp);
	server.client().stop();
}

void loop(void) {
	loop_ota();
	server.handleClient();
	temphumLoopHandler();
	delay(10);
}
