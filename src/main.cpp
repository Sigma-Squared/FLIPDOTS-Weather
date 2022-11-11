#include <Arduino.h>
#include <WiFi.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "FLIPDOTS.h"
#include "GOL.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

#define API_KEY "<YOUR API KEY>"

#define DEBUG 1
#if DEBUG
#define DEBUGLOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__);
#else
#define DEBUGLOG(fmt, ...)
#endif

#define WIFI_TIMEOUT 20000
#define BT_TIMEOUT 10000
#define SSID_MAXLEN 32
#define WIFI_PASS_MAXLEN 64
#define ZIP_MAXLEN 16
#define UPDATE_FREQ 1800000 // 30 mins
#define RETRY_AFTER 60000   // 1 min
#define FORECAST_LOOKAHEAD 2

#define LOADINGSCREEN_GENERIC 0
#define LOADINGSCREEN_BLUETOOTH 1
#define LOADINGSCREEN_GLIDER 2

#define CONFIGURED_VIA_BT 0
#define CONFIGURED_VIA_NVS 1

FLIPDOTS display(&Serial2);
BluetoothSerial SerialBT;
Preferences preferences;
char zipCode[ZIP_MAXLEN] = "";
char weatherUrl[128] = "";
char forecastUrl[128] = "";

void displayError()
{
    const byte errorDisplay[] = {
        0b00000000,
        0b00001000,
        0b00000000,
        0b00001000,
        0b00001000,
        0b00001000,
        0b00000000};
    display.write(errorDisplay);
}

void taskDisplayLoader(void *params)
{
    uint8_t type = (long)params;
    switch (type)
    {
    case LOADINGSCREEN_BLUETOOTH:
    {
        byte buffer[7] = {0};
        byte frame[7] = {
            0b00000000,
            0b00001000,
            0b00001100,
            0b00001000,
            0b00001100,
            0b00001000,
            0b00000000};
        while (true)
        {
            display.write(frame);
            vTaskDelay(250);
            display.write(buffer);
            vTaskDelay(250);
        };
    }
    case LOADINGSCREEN_GLIDER:
    {
        byte buffer[7] = {
            0b00000000,
            0b00000000,
            0b00000000,
            0b00000000,
            0b00110000,
            0b01010000,
            0b00010000};
        while (true)
        {
            display.write(buffer);
            GOL(buffer);
            vTaskDelay(250);
        }
    }
    case LOADINGSCREEN_GENERIC:
    default:
    {
        byte buffer[7] = {0};
        while (true)
        {
            buffer[3] = 0b00010000;
            display.write(buffer);
            vTaskDelay(250);
            buffer[3] = buffer[3] >> 1;
            display.write(buffer);
            vTaskDelay(250);
            buffer[3] = buffer[3] >> 1;
            display.write(buffer);
            vTaskDelay(250);
        }
    }
    }
}

bool displayWeather()
{
    HTTPClient http;
    http.useHTTP10();

    // Current weather
    http.begin(weatherUrl);
    int responseCode = http.GET();
    if (responseCode != 200)
    {
        DEBUGLOG("Failed to fetch current weather. Response code: %u\n", responseCode);
        return false;
    }
    DynamicJsonDocument jsonDoc(2048);
    DeserializationError error = deserializeJson(jsonDoc, http.getStream());
    if (error)
    {
        DEBUGLOG("Deserialization Error\n");
        return false;
    }

    JsonVariant mainTemp = jsonDoc["main"]["feels_like"];
    JsonVariant mainHumidity = jsonDoc["main"]["humidity"];
    JsonVariant currentVisibility = jsonDoc["visibility"];
    if (mainTemp.isNull() || mainHumidity.isNull() || currentVisibility.isNull())
    {
        DEBUGLOG("Missing data from current weather.\n");
        return false;
    }
    int currentTemp = lround(mainTemp.as<double>() - 273.15);
    double humidity = mainHumidity.as<double>() / 100.0;
    double visibility = currentVisibility.as<double>() / 10000.0;

    // Forecast
    http.begin(forecastUrl);
    responseCode = http.GET();
    if (responseCode != 200)
    {
        DEBUGLOG("Failed to fetch forecast. Response code: %d\n", responseCode);
        return false;
    }
    error = deserializeJson(jsonDoc, http.getStream());
    if (error)
    {
        DEBUGLOG("Deserialization Error\n");
        return false;
    }
    double maxPop = 0.0;
    for (uint8_t i = 0; i < FORECAST_LOOKAHEAD; i++)
    {
        JsonVariant itemPop = jsonDoc["list"][i]["pop"];
        if (itemPop.isNull())
        {
            DEBUGLOG("Precipitation percentage forecast not found.\n");
            return false;
        }
        double itemPopAmt = itemPop.as<double>();
        if (itemPopAmt > maxPop)
        {
            maxPop = itemPopAmt;
        }
    }

    DEBUGLOG("Current temp: %d, humidity %f, visibility %f. Forecast max pop: %f", currentTemp, humidity, visibility, maxPop);
    char charArray[2] = "";
    sprintf(charArray, "%2d", currentTemp < 0 ? -currentTemp : currentTemp);
    display.write3x3char2andBars(charArray, maxPop, humidity, visibility, currentTemp < 0);
    return true;
}

void taskUpdateDisplay(void *params)
{
    // we are now synchronized with minute change, run every 60s
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (true)
    {
        if (displayWeather())
        {
            vTaskDelayUntil(&lastWakeTime, UPDATE_FREQ);
        }
        else
        {
            vTaskDelayUntil(&lastWakeTime, RETRY_AFTER);
        }
    }
}

void cleanInput(char *input)
{
    // remove leading and trailing whitespace
    uint len = strlen(input);
    uint start = 0;
    while (isspace(input[start++]))
        ;
    memmove(input, input + start - 1, len - start + 2);
    for (uint end = len - 1; isspace(input[end]); end--)
    {
        input[end] = '\0';
    }
}

void setUrlsFromZipCode()
{
    sprintf(weatherUrl, "http://api.openweathermap.org/data/2.5/weather?zip=%s&appid=%s", zipCode, API_KEY);
    sprintf(forecastUrl, "http://api.openweathermap.org/data/2.5/forecast?zip=%s&appid=%s&cnt=%d", zipCode, API_KEY, FORECAST_LOOKAHEAD);
    DEBUGLOG("query URLs:\n%s\n%s\n", weatherUrl, forecastUrl);
}

uint8_t getCredentialsViaBluetoothOrNVS(char *ssid, char *password)
{
    // Attempt to receive WiFI credentials via Bluetooth
    DEBUGLOG("Connecting Bluetooth");
    SerialBT.begin("FLIPDOTS Clock");
    long startTime = millis();
    while (!SerialBT.connected() && (millis() - startTime) < BT_TIMEOUT)
    {
        DEBUGLOG(".");
        delay(500);
    }
    if (SerialBT.connected()) // Configure via Bluetooth
    {
        DEBUGLOG("Connected\n");
        SerialBT.setTimeout(100000);

        SerialBT.println("Please enter WIFI SSID:");
        SerialBT.readBytesUntil('\r', ssid, SSID_MAXLEN * sizeof(char));
        cleanInput(ssid);

        SerialBT.println("Please enter WIFI password:");
        SerialBT.readBytesUntil('\r', password, WIFI_PASS_MAXLEN * sizeof(char));
        cleanInput(password);

        SerialBT.println("Please enter your ZIP code:");
        SerialBT.readBytesUntil('\r', zipCode, ZIP_MAXLEN * sizeof(char));
        cleanInput(zipCode);

        SerialBT.printf("Using WiFi \"%s\" with credential \"%s\", ZIP Code %s. Disconnecting.\n", ssid, password, zipCode);

        delay(500);
        SerialBT.end();
        return CONFIGURED_VIA_BT;
    }
    else // Didn't connect, configure via stored information on disk instead
    {
        SerialBT.end();
        DEBUGLOG("Didn't sync via Bluetooth. Loading credentials from NVS instead.\n");
        preferences.getBytes("ssid", ssid, SSID_MAXLEN * sizeof(char));
        preferences.getBytes("password", password, WIFI_PASS_MAXLEN * sizeof(char));
        preferences.getBytes("zipcode", zipCode, ZIP_MAXLEN * sizeof(char));
        return CONFIGURED_VIA_NVS;
    }
}

bool connectWiFi(const char *ssid, const char *password)
{
    // Connect to WiFi
    WiFi.mode(WIFI_STA);
    DEBUGLOG("Connecting Wifi");
    WiFi.begin(ssid, password);
    long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_TIMEOUT)
    {
        DEBUGLOG(".");
        delay(500);
    }
    if (WiFi.status() != WL_CONNECTED) // Couldn't connect to WiFi
    {
        DEBUGLOG("Failed.\n");
        return false;
    }
    DEBUGLOG("Connected (%s).\n", WiFi.localIP().toString());
    return true;
}

void setup()
{
#if DEBUG
    Serial.begin(115200);
    Serial.setDebugOutput(true);
#endif
    pinMode(LED_BUILTIN, OUTPUT);
    display.begin();
    preferences.begin("flipdots", false);
    TaskHandle_t showLoaderTask;
    char ssid[SSID_MAXLEN] = "";
    char password[WIFI_PASS_MAXLEN] = "";

    xTaskCreate(taskDisplayLoader, "taskDisplayLoader", 1024, (void *)LOADINGSCREEN_BLUETOOTH, 1, &showLoaderTask);
    uint8_t configuredFrom = getCredentialsViaBluetoothOrNVS(ssid, password);
    vTaskDelete(showLoaderTask);

    if (strlen(ssid) == 0 || strlen(password) == 0 || strlen(zipCode) == 0) // No credentials
    {
        DEBUGLOG("Failed to load WiFi credentials from anywhere.\n");
        displayError();
        while (1)
            ;
    }

    // Show loading screen & connect Wifi & configure time
    xTaskCreate(taskDisplayLoader, "taskDisplayLoader", 1024, (void *)LOADINGSCREEN_GLIDER, 1, &showLoaderTask);
    digitalWrite(LED_BUILTIN, HIGH);
    DEBUGLOG("Using WiFi \"%s\" with credential \"%s\", ZIP code %s\n", ssid, password, zipCode);
    setUrlsFromZipCode();
    if (!connectWiFi(ssid, password))
    {
        vTaskDelete(showLoaderTask);
        digitalWrite(LED_BUILTIN, LOW);
        displayError();
        while (1)
            ;
    }
    // WiFi success, save credentials in non-volatile storage for next boot (unless loaded from non-volatile storage)
    if (configuredFrom != CONFIGURED_VIA_NVS)
    {
        preferences.putBytes("ssid", (const char *)ssid, strlen(ssid));
        preferences.putBytes("password", (const char *)password, strlen(password));
        preferences.putBytes("zipcode", (const char *)zipCode, strlen(zipCode));
    }
    preferences.end();
    digitalWrite(LED_BUILTIN, LOW);
    vTaskDelete(showLoaderTask);

    // short animation, then show time
    display.setInverted(true);
    display.clear();
    display.setInverted(false);
    delay(250);
    xTaskCreate(taskUpdateDisplay, "taskUpdateDisplay", 4096, NULL, 1, NULL);
}

void loop()
{
    vTaskSuspend(NULL);
}