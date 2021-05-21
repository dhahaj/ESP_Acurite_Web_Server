/* ====== ESP8266 Demo ======
   Print out analog values
   (Updated Dec 14, 2014)
   ==========================

   Change SSID and PASS to match your WiFi settings.
   The IP address is displayed to soft serial upon successful connection.

   Ray Wang @ Rayshobby LLC
   http://rayshobby.net/?p=9734
*/

// comment this part out if not using LCD debug
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include "secrets.h"

#define RING_BUFFER_SIZE  256

#define SYNC_LENGTH 2200

#define SYNC_HIGH  600
#define SYNC_LOW   600
#define BIT1_HIGH  400
#define BIT1_LOW   220
#define BIT0_HIGH  220
#define BIT0_LOW   400

#define BUFFER_SIZE 512

#define PORT  80                // using port 8080 by default
#define DATAPIN D2

char buffer[BUFFER_SIZE];
const char* ssid = SSID;
const char* password = PASS;
volatile unsigned long _temp, _hum;

ESP8266WebServer server(PORT);

unsigned long timings[RING_BUFFER_SIZE];
unsigned int syncIndex1 = 0,  // index of the first sync signal
             syncIndex2 = 0;  // index of the second sync signal
bool received = false;
uint32_t lastTime = 0;
bool flag = true;

// If using Software Serial for debug
// Use the definitions below
//#include <SoftwareSerial.h>
//SoftwareSerial dbg(7, 8); // use pins 7, 8 for software serial
//#define esp Serial


// If your MCU has dual USARTs (e.g. ATmega644)
// Use the definitions below
//#define dbg Serial    // use Serial for debug
//#define esp Serial   // use Serial1 to talk to esp8266

void handleRoot()
{
  char temp[400];
  snprintf(temp, 400, "<html>"
           "<head>"
           "<meta http-equiv='refresh' content='5' /> "
           "<title>ESP8266 RF Sniffer</title> <style>"
           "body { background - color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }"
           "</style> </head> <body>"
           "<h1>Hello from ESP8266!</h1>"
           "<p>Temperature: % 02dºF</p>"
           "<p>Humidity: % 02d%%RH</p>"
           "<img src=\"/test.svg\"/></body></html>",
           _temp, _hum);
  server.send(200, "text/html", temp);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++)
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";

  server.send(404, "text/plain", message);
}

void setupWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

// detect if a sync signal is present
bool isSync(unsigned int idx)
{
  // check if we've received 4 squarewaves of matching timing
  int i;
  for (i = 0; i < 8; i += 2)
  {
    unsigned long t1 = timings[(idx + RING_BUFFER_SIZE - i) % RING_BUFFER_SIZE];
    unsigned long t0 = timings[(idx + RING_BUFFER_SIZE - i - 1) % RING_BUFFER_SIZE];
    if (t0 < (SYNC_HIGH - 100) || t0 > (SYNC_HIGH + 100) || t1 < (SYNC_LOW - 100)  || t1 > (SYNC_LOW + 100))
      return false;
  }

  // check if there is a long sync period prior to the 4 squarewaves
  unsigned long t = timings[(idx + RING_BUFFER_SIZE - i) % RING_BUFFER_SIZE];

  if (t < (SYNC_LENGTH - 400) || t > (SYNC_LENGTH + 400) || digitalRead(DATAPIN) != HIGH)
    return false;

  return true;
}

ICACHE_RAM_ATTR void ISR()
{
  static unsigned long duration = 0, lastTime = 0;
  static unsigned int ringIndex = 0, syncCount = 0;

  // ignore if we haven't processed the previous received signal
  if (received == true)
    return;

  // calculating timing since last change
  long time = micros();
  duration = time - lastTime;
  lastTime = time;

  // store data in ring buffer
  ringIndex = (ringIndex + 1) % RING_BUFFER_SIZE;
  timings[ringIndex] = duration;

  // detect sync signal
  if (isSync(ringIndex))
  {
    syncCount ++;
    // first time sync is seen, record buffer index
    if (syncCount == 1)
      syncIndex1 = (ringIndex + 1) % RING_BUFFER_SIZE;
    else if (syncCount == 2)
    {
      // second time sync is seen, start bit conversion
      syncCount = 0;
      syncIndex2 = (ringIndex + 1) % RING_BUFFER_SIZE;
      unsigned int changeCount = (syncIndex2 < syncIndex1) ? (syncIndex2 + RING_BUFFER_SIZE - syncIndex1) : (syncIndex2 - syncIndex1);
      // changeCount must be 122 -- 60 bits x 2 + 2 for sync
      if (changeCount != 122)
      {
        received = false;
        syncIndex1 = 0;
        syncIndex2 = 0;
      }
      else received = true;
    }
  }
}


int t2b(unsigned int t0, unsigned int t1)
{
  if (t0 > (BIT1_HIGH - 100) && t0 < (BIT1_HIGH + 100) && t1 > (BIT1_LOW - 100) && t1 < (BIT1_LOW + 100))
    return 1;
  else if (t0 > (BIT0_HIGH - 100) && t0 < (BIT0_HIGH + 100) && t1 > (BIT0_LOW - 100) && t1 < (BIT0_LOW + 100))
    return 0;
  return -1;  // undefined
}

void handleReceive()
{
  detachInterrupt(digitalPinToInterrupt(3));
  // extract humidity value
  unsigned long humidity, temp;
  unsigned int startIndex, stopIndex;
  bool fail = false;
  startIndex = (syncIndex1 + (3 * 8 + 1) * 2) % RING_BUFFER_SIZE;
  stopIndex =  (syncIndex1 + (3 * 8 + 8) * 2) % RING_BUFFER_SIZE;

  for (int i = startIndex; i != stopIndex; i = (i + 2) % RING_BUFFER_SIZE)
  {
    int bit = t2b(timings[i], timings[(i + 1) % RING_BUFFER_SIZE]);
    humidity = (humidity << 1) + bit;
    if (bit < 0)
      fail = true;
  }


  if (fail)
    Serial.println(F("Decoding error."));
  else {
    Serial.print(F("Humidity: "));
    Serial.print(humidity);
    Serial.print("\% / ");
  }

  fail = false;

  // most significant 4 bits
  startIndex = (syncIndex1 + (4 * 8 + 4) * 2) % RING_BUFFER_SIZE;
  stopIndex  = (syncIndex1 + (4 * 8 + 8) * 2) % RING_BUFFER_SIZE;
  for (int i = startIndex; i != stopIndex; i = (i + 2) % RING_BUFFER_SIZE)
  {
    int bit = t2b(timings[i], timings[(i + 1) % RING_BUFFER_SIZE]);
    temp = (temp << 1) + bit;
    if (bit < 0)  fail = true;
  }

  // least significant 7 bits
  startIndex = (syncIndex1 + (5 * 8 + 1) * 2) % RING_BUFFER_SIZE;
  stopIndex  = (syncIndex1 + (5 * 8 + 8) * 2) % RING_BUFFER_SIZE;
  for (int i = startIndex; i != stopIndex; i = (i + 2) % RING_BUFFER_SIZE)
  {
    int bit = t2b(timings[i], timings[(i + 1) % RING_BUFFER_SIZE]);
    temp = (temp << 1) + bit;
    if (bit < 0)
      fail = true;
  }

  if (fail)
    Serial.println(F("Decoding error."));
  else
  {
    Serial.print(F("Temperature: "));
    Serial.print((int)((temp - 1024) / 10 + 1.9 + 0.5)); // round to the nearest integer
    Serial.write(176);    // degree symbol
    Serial.print(F("C/"));
    Serial.print((int)(((temp - 1024) / 10 + 1.9 + 0.5) * 9 / 5 + 32)); // convert to F
    Serial.write(176);    // degree symbol
    Serial.println(F("F"));
  }

  // delay for 1 second to avoid repetitions
  delay(1000);
  received = false;
  syncIndex1 = 0;
  syncIndex2 = 0;

  // re-enable interrupt
  attachInterrupt(digitalPinToInterrupt(3), ISR, CHANGE);

}

void setup()
{
  Serial.begin(115200);

  setupWiFi();

  Serial.println(WiFi.localIP());
  pinMode(D2, INPUT);
  attachInterrupt(digitalPinToInterrupt(D2), ISR, CHANGE);

}

void loop()
{
  if (received == true)
  {
    handleReceive();
  }
  server.handleClient();
  MDNS.update();
}
