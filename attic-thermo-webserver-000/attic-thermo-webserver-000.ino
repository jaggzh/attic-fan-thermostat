#define USE_DALLAS      // Using DS18B20?
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
#include "FS.h"
#endif

#ifdef USE_DALLAS
#include <OneWire.h>
#include <DallasTemperature.h>
#define DALPIN D3   // data pin for ds18b20
OneWire oneWire(DALPIN); // on our pin (bus), but for any onewire devices
DallasTemperature ds18sensors(&oneWire); // Pass our oneWire ref to Dallas Temperature. 
#endif

//#include <ArduinoOTA.h>
#include "wifi_config.h" // This sets actual values (gross): ssid, password. ip, gw, nm


#include "DHT.h"
#define RELAYPIN  D1  // D1 = ? maybe find out.. or maybe who cares
#define DHTPIN  2     // D4 = 2 (D4, 2, is shield's pin)
#define DHTTYPE DHT11 // DHT11

#define VERBOSE 2

#define MAX_STR_SEND_LEN 2000

//#include "time.c" // including a .c, yay!  ssid, password.  ip, gw, nm

#define sp(a) Serial.print(a)
#define sl(a) Serial.println(a)

#define TYPE_DEGF 1  // Graph types: Degrees / humidity
#define TYPE_HUM  2
#define GET_TEMP_ATTEMPTS 3 // attempts to make reading therm
#define ROOT_MAX_HTML 800    // size of HTML buffer
#define SMALL_HTML    300    // size of HTML buffer
// Circle Buffer: Last-n (data array, nextVal, total vals, offset (n))
// Example: tdp = &CB_PRIOR_N(dayData, dayNext, DAY_DATAPOINTS, 0);
//          reads last entry.  n=1 would read the one before that
#define CB_PRIOR_N(dat, nxtv, tot, n) (dat[nxtv-n>0 ? nxtv-n-1 : tot-n-1])

unsigned long time_last = 0;
ESP8266WebServer server(80); //main web server
ESP8266HTTPUpdateServer httpUpdater;
const int led = 13;
DHT dht(DHTPIN, DHTTYPE);

struct temphum_data_store {
	#ifdef USE_DHT11
	float h, f;
	#endif
	#ifdef USE_DALLAS
	float df;
	#endif
};
struct temphum_data {
	#ifdef USE_DHT11
	float h, c, f, hic, hif;
	char hs[7], cs[7], fs[7], hics[7], hifs[7];
	#endif
	#ifdef USE_DALLAS
	float df; // for dallas reading degf
	char dfs[7];
	#endif
};
#define DEF_FAN_TEMP  110 // some days it's even hotter than this always though
#define FN_FANTEMP "/f.txt"
#define FAN_MIN_SECS 60
#define FAN_THRESH 1    // turn off after fanTemp-this_value

#define DAY_FREQS 60  // seconds
#define MON_DATAPOINTS (24*30)              // One per hour
#define DAY_DATAPOINTS (24*60*60/DAY_FREQS) // Every minute
//#define DAY_DATAPOINTS (24*15)
#define WEB_REFRESH_SECS "30"   // Seconds for webpage refresh, as a string
int lastFanChange = 0;
int relaystate=LOW;
int fanOnTemp=DEF_FAN_TEMP;
// longterm data: Add 1 just in case we're dumb and overrun
struct temphum_data_store dayData[DAY_DATAPOINTS+1];
int dayStart=0;
int dayNext=0;
#ifdef USE_DALLAS
//uint8_t dal_addr=0; // this address method might be faster than byindex
#endif

void reset_lasttime(void) {
	time_last = millis()/1000;
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
int get_temphum_floats(struct temphum_data *td) {
#ifdef USE_DALLAS
	float res;
	ds18sensors.requestTemperatures();
	res = ds18sensors.getTempFByIndex(0);
	char temp[SMALL_HTML];
	snprintf(temp, SMALL_HTML, "DS18 DegF: %.8f floor=>%.8f int=>%d", res, floor(res), (int)res);
	sl(temp);
	// Disconnected DS18B20 data line yields DEVICE_DISCONNECTED_F
	// Disc. power lead yields 185 (also from getTempFbyindex())
	if ((int)res == -196 || (int)res == 185) {
		sl("DS18B20[0] disconnected");
		return 1;
	}
	td->df = res;
#endif
#ifdef USE_DHT11
	td->h = dht.readHumidity(1); // force read
	td->c = dht.readTemperature(false, 1); // celsius, (force it)
	td->f = dht.readTemperature(true, 1);  // fahrenheit, (force it)
	if (isnan(td->h)) { // extra tests just for more verbose reporting
		Serial.print("Failed hum read\n");
	}
	if (isnan(td->c) || isnan(td->f)) {
		Serial.print("Failed temp read\n");
	}
	if (isnan(td->h) || isnan(td->c) || isnan(td->f)) {
		//Serial.print("Failed to read hum, degc, or degf\n");
		return 1;
	}
	td->hic = dht.computeHeatIndex(td->c, td->h, false);       
	td->hif = dht.computeHeatIndex(td->f, td->h);       
	// You can delete the following Serial.print's, it's just for debugging purposes
	Serial.print("Hum: ");
	Serial.print(td->h);
	Serial.print(" %\t Temp: ");
	Serial.print(td->c);
	Serial.print(" *C ");
	Serial.print(td->f);
	Serial.print(" *F\t Heat index: ");
	Serial.print(td->hic);
	Serial.print(" *C ");
	Serial.print(td->hif);
	Serial.print(" *F\n");
#endif
	return 0;
}

#if 0 // not using strings
int float_to_s7(char *str, float f) {
	dtostrf(f, 6, 2, str); 
}

int get_temphum_all(struct temphum_data *td) {
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
	struct temphum_data td;
	int succ=0;
	// try a lot of times to get a reading
	sl("Reading initial temp data");

	for (int i=0; i<GET_TEMP_ATTEMPTS; i++) {
		if (!get_temphum_floats(&td)) {
			succ=1;
			break; // success
		}
		sl("Failed initial read, trying 3 times\n");
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
		#endif
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
	server.send(200, "text/html", temp );
}
#endif

void refresh_send(int secs, char *url, char *htmlopt) {
	char temp[SMALL_HTML];
	int len;
	len = snprintf(temp, SMALL_HTML,
		"<html><head><meta http-equiv=refresh content='%d; url=%s' /></head><body>%s</body></html>",
		secs, url, htmlopt ? htmlopt : ""
	);
	server.send(200, "text/html", temp);
}

void fanOnHTTP() {
	fanOn();
	refresh_send(2, "/", (char *)F("Turned on"));
}
void fanOffHTTP() {
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
	int sec = millis() / 1000;
	int min = sec / 60;
	int hr = min / 60;
	struct temphum_data_store *td;
	float minf, maxf;
	float minh, maxh;
	String out = "";

	relaystate = digitalRead(RELAYPIN);
	sp("Fan relay current state: "); sl(relaystate);

	get_day_minmax(&minf, &maxf, TYPE_DEGF); // handles dallas and dht11
	get_day_minmax(&minh, &maxh, TYPE_HUM);  // invalid results if no dht11
	//int i = dayNext-1>0 ? dayNext-2 : DAY_DATAPOINTS-2;
#ifdef USE_DALLAS
	sp("DS18B20 DegF: ");
	for (int i=0; i<DAY_DATAPOINTS; i+=int(DAY_DATAPOINTS/15)) {
		td = &CB_PRIOR_N(dayData, dayNext, DAY_DATAPOINTS, i);
		sp(td->df); sp(" ");
	}
	sp("\n");
#endif
#ifdef USE_DHT11
	sp("DHT11 DegF: ");
	for (int i=0; i<DAY_DATAPOINTS; i+=int(DAY_DATAPOINTS/15)) {
		td = &CB_PRIOR_N(dayData, dayNext, DAY_DATAPOINTS, i);
		sp(td->f); sp(" ");
	}
	sp("\nHum: ");
	for (int i=0; i<DAY_DATAPOINTS; i+=int(DAY_DATAPOINTS/15)) {
		td = &CB_PRIOR_N(dayData, dayNext, DAY_DATAPOINTS, i);
		sp(td->h); sp(" ");
	}
	sp("\n");
#endif
	td = &CB_PRIOR_N(dayData, dayNext, DAY_DATAPOINTS, 0);
	snprintf(temp, ROOT_MAX_HTML,
		"<html>"
		"<head>"
		"<meta http-equiv=refresh content=" WEB_REFRESH_SECS " />"
		"<title>ESP8266</title>"
		"<style>"
		"body{background:#eee;font-family:Sans-Serif;color:#008;font-size:150%%;}"
		"img{background:MidnightBlue;margin:0 auto;}"
		"</style>"
		"</head>"
		"<body>"
		"<p>Uptime: %02d:%02d:%02d [<a href=/update>Update</a>]<br/>"
		"Fan on @ %d degF<br/>"
		"Fan state: %s [<a href=fon>On</a>] [<a href=foff>Off</a>]<br/>"
		"",
		hr, min % 60, sec % 60,
		fanOnTemp,
		relaystate == LOW ? "OFF" : "ON");
	out += temp;
#ifdef USE_DHT11
	snprintf(temp, ROOT_MAX_HTML,
		"DHT DegF: %.2f (min: %.2f, max: %.2f)<br />"
		"DHT Hum: %.2f, Min: %.2f Max: %.2f<br />"
		"",
		td->f, minf, maxf, td->h, minh, maxh);
	out += temp;
#endif
#ifdef USE_DALLAS
	snprintf(temp, ROOT_MAX_HTML,
		"DS18 DegF: %.2f (min: %.2f, max: %.2f)<br />"
		"",
		td->df, minf, maxf);
	out += temp;
#endif
	snprintf(temp, ROOT_MAX_HTML,
		"XRange %dh%dm%ds. %d total samples, every %ds</p>"
		"<img src=/f.svg />"
		"",
		int(DAY_DATAPOINTS * DAY_FREQS / 60 / 60),   // all ints anyway
		int((DAY_DATAPOINTS * DAY_FREQS / 60)) % 60, // have to be sure with %
		int(DAY_DATAPOINTS * DAY_FREQS) % 60,
		DAY_DATAPOINTS,
		DAY_FREQS);
	out += temp;
#ifdef USE_DHT11
	out += F("<img src=/h.svg />");
#endif
	snprintf(temp, ROOT_MAX_HTML,
		"<form action=sett><input type=number name=n value=%d><input type=submit value=Set></form>"
		"</body></html>"
		"",
		fanOnTemp);
	out += temp;
	server.send ( 200, "text/html", out );
	digitalWrite ( led, 0 );
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
	SPIFFS.begin();
	File f = SPIFFS.open(FN_FANTEMP, "r");
	if (!f) sl(F("open failed")), fanOnTemp=DEF_FAN_TEMP;
	else {
		fanOnTemp = f.parseInt();
		sp(F("Loaded temp: "));
		sl(fanOnTemp);
		f.close();
	}
	SPIFFS.end();
}

void setTempTrigger(void) {
	String ns=server.arg("n");
	char temp[110];
	if (!isdigit(ns[0])) refresh_send(5, "/", "BAD NUMBER!");
	else {
		SPIFFS.begin();
		File f = SPIFFS.open(FN_FANTEMP, "w");
		if (!f) sl("open error");
		else {
			fanOnTemp = ns.toInt();
			f.print(fanOnTemp);
			f.close();
			refresh_send(2, "/", "Set temp");
			sp("set temp to ");
			sl(fanOnTemp);
		}
		SPIFFS.end();
	}
}

void setup ( void ) {
#ifdef INIT_SPIFFS
	Serial.println(F("Wait 30 secs SPIFFS format"));
	SPIFFS.format();
	Serial.println(F("Spiffs formatted"));
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
	WiFi.mode(WIFI_STA);
	Serial.begin(115200);
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
	server.on(F("/sett"), setTempTrigger );
	//server.on("/inline", []() {server.send(200,"text/plain","this works as well"); });
	httpUpdater.setup(&server, update_user, update_pw); // adds /update path for OTA
	server.onNotFound ( handleNotFound );
	server.begin();
	Serial.println ( F("HTTP svr started") );

#ifdef SAVE_SPIFFS
	//SPIFFS.begin();
#endif
}

// Gets temperature, puts into struct at tdp.
// Then stores log values to dayData
// Increments to next sensor storage location
// Does NOT increment if read error from sensor(s)
int get_and_store_temp(struct temphum_data *tdp) {
	int rc=1;
	struct temphum_data_store *storep;
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

void temphumLoopHandler(void) {
	unsigned long timeNow;
	int seconds;
	struct temphum_data td;

	timeNow = millis() / 1000;	// the number of milliseconds that have passed since boot
	seconds = timeNow - time_last;	//the number of seconds that have passed since the last time 60 seconds was reached.

	if (seconds >= DAY_FREQS) {   // at or past time to get temperature
		time_last = timeNow;  // Reset timer even if we don't get the data
			// Try twice to get temp data
		if (!get_and_store_temp(&td) || !get_and_store_temp(&td)) {
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
			if (timeNow - lastFanChange > FAN_MIN_SECS) {
				if (curf >= fanOnTemp && relaystate == LOW) {
					sl(F("Fan turned ON"));
					lastFanChange = timeNow;
					fanOn();
				} else if (curf < fanOnTemp-FAN_THRESH && relaystate == HIGH) {
					sl(F("Fan turned OFF"));
					lastFanChange = timeNow;
					fanOff();
				}
			}
		}
	}
}

void loop ( void ) {
	//ArduinoOTA.handle();
	//delay(10);
	temphumLoopHandler();
	server.handleClient();
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
#ifdef USE_DHT11
	sp("GRAPH DHT: ");
	n=0, i=dayNext+1;
	for (; n<DAY_DATAPOINTS; n+=(DAY_DATAPOINTS/16), i+=(DAY_DATAPOINTS/16)) {
		struct temphum_data_store *ts;
		if (i>DAY_DATAPOINTS) i=0;
		ts = &dayData[i];
		sp(ts->f);
		sp(" ");
	}
	sl("");
#endif
#ifdef USE_DALLAS
	sp("GRAPH DS18: ");
	n=0, i=dayNext+1;
	for (; n<DAY_DATAPOINTS; n+=(DAY_DATAPOINTS/16), i+=(DAY_DATAPOINTS/16)) {
		struct temphum_data_store *ts;
		if (i>DAY_DATAPOINTS) i=0;
		ts = &dayData[i];
		sp(ts->df);
		sp(" ");
	}
	sl("");
#endif
#define WIDTH  380
#define HEIGHT 130
#define PAD    10
#define PAD2   (PAD*2)
	//void ESP8266WebServer::_prepareHeader(String& response, int code, const char* content_type, size_t contentLength) {
	String response = "";
	//server._prepareHeader(response, 200, "image/svg+xml", CONTENT_LENGTH_UNKNOWN);
	server.sendContent("HTTP/1.0 200 OK\r\n");
	server.sendContent("Content-Type: image/svg+xml\r\n\r\n");
	//server.setContentLength(CONTENT_LENGTH_UNKNOWN);
	//server.send ( 200, "image/svg+xml", "");
	sprintf(temp, "<svg xmlns='http://www.w3.org/2000/svg' version='1.1' width='%d' height='%d'>\n", WIDTH+PAD2, HEIGHT+PAD2);
	server.sendContent(temp);
	server.sendContent(
		F("<defs>"
		"<style type='text/css'><![CDATA["
		"line{stroke-width:2;}"
		".hg{stroke-width:1;stroke:gray;}"
		".vg{stroke-width:1;stroke:#315A5C;}"
		"]]></style>"
		"</defs>"));
 	//out += temp;

	// Get minimum and maximum temps
	float minv, maxv;
	get_day_minmax(&minv, &maxv, type);
	//sp("Min max: "); sp(minv); sp(" -> "); sl(maxv);
	minv=floor(minv);
	maxv=ceil(maxv);
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
	struct temphum_data_store *td1, *td2;
	// go from: dayNext -> DAY_DATAPOINTS-1
	//    then: 0 to dayNext-1
	//if (i<0) i=DAY_DATAPOINTS-1;
	// ((360 * 120 = total secs) / 60 / 60 = total hours) = 12
	int xlines = (DAY_DATAPOINTS * DAY_FREQS)/60/60;
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
			if (out.length()+strlen(temp) >= MAX_STR_SEND_LEN) {
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
	out = "</svg>\n";
	server.sendContent(out);
}
