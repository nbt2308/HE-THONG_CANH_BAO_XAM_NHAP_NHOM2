
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Arduino.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp_http_server.h"

const char* ssid = "";
const char* password = "";

const char* botToken = "";
const char* chatID = "";

WiFiClientSecure telegramClient;



#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// CAMERA WEB SERVER
// Implemented in app_httpd.cpp
void startCameraServer();

// ==================================================
// PIR CONFIGURATION
// ==================================================
#define FLASH_LED_PIN 4
#define PIR_PIN 3

const unsigned long PIR_WARMUP_TIME = 30000;
const unsigned long MOTION_COOLDOWN = 3000;

unsigned long startupTime = 0;
unsigned long lastMotionTime = 0;

bool cameraReady = false;

// CAMERA MUTEX
SemaphoreHandle_t cameraMutex = NULL;


// CAMERA INITIALIZATION
bool initializeCamera() {

    Serial.println("\nInitializing camera...");

    Serial.printf(
        "PSRAM found: %s\n",
        psramFound() ? "YES" : "NO"
    );

    Serial.printf(
        "PSRAM size: %u bytes\n",
        ESP.getPsramSize()
    );

    Serial.printf(
        "Free PSRAM: %u bytes\n",
        ESP.getFreePsram()
    );

    Serial.printf(
        "Free heap: %u bytes\n",
        ESP.getFreeHeap()
    );

    camera_config_t config = {};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // Memory-efficient camera configuration
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;

    if (psramFound()) {
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size = FRAMESIZE_QQVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);

    if (err != ESP_OK) {
        Serial.printf(
            "Camera initialization failed: 0x%x\n",
            err
        );

        return false;
    }

    sensor_t* sensor = esp_camera_sensor_get();

    if (sensor != NULL) {
        sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        sensor->set_quality(sensor, 12);
    }

    Serial.println("Camera initialized successfully!");

    return true;
}

// CAPTURE PHOTO
camera_fb_t* capturePhoto() {

    camera_fb_t* fb = NULL;

    // Loại bỏ các frame cũ đang chờ
    for (int i = 0; i < 3; i++) {
        camera_fb_t* oldFb = esp_camera_fb_get();

        if (oldFb != NULL) {
            esp_camera_fb_return(oldFb);
        }

        delay(100);
    }

    // Lấy frame mới nhất
    for (int attempt = 1; attempt <= 3; attempt++) {

        fb = esp_camera_fb_get();

        if (fb != NULL && fb->len > 0) {

            Serial.printf(
                "New frame captured: %u bytes\n",
                (unsigned int)fb->len
            );

            return fb;
        }

        if (fb != NULL) {
            esp_camera_fb_return(fb);
            fb = NULL;
        }

        Serial.printf("Capture attempt %d failed!\n", attempt);

        delay(200);
    }

    Serial.println("Unable to capture fresh image!");

    return NULL;
}


// SEND PHOTO TO TELEGRAM
bool sendPhotoTelegram(camera_fb_t* fb) {

    if (fb == NULL || fb->len == 0) {
        Serial.println("Invalid image!");
        return false;
    }

    const char* host = "api.telegram.org";

    telegramClient.setInsecure();
    telegramClient.setTimeout(15000);

    Serial.println("Connecting to Telegram...");

    if (!telegramClient.connect(host, 443)) {
        Serial.println("Telegram connection failed!");
        return false;
    }

    String boundary = "----ESP32CAMBoundary";

    String head =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
        String(chatID) + "\r\n" +
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"capture.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";

    String tail =
        "\r\n--" + boundary + "--\r\n";

    size_t imageLength = fb->len;

    size_t totalLength =
        head.length() + imageLength + tail.length();

    telegramClient.println(
        "POST /bot" + String(botToken) +
        "/sendPhoto HTTP/1.1"
    );

    telegramClient.println("Host: api.telegram.org");

    telegramClient.println(
        "Content-Type: multipart/form-data; boundary=" +
        boundary
    );

    telegramClient.println(
        "Content-Length: " + String(totalLength)
    );

    telegramClient.println("Connection: close");
    telegramClient.println();

    telegramClient.print(head);

    // Upload image in chunks
    size_t sent = 0;

    while (sent < imageLength) {

        size_t chunkSize = min(
            (size_t)1024,
            imageLength - sent
        );

        size_t written = telegramClient.write(
            fb->buf + sent,
            chunkSize
        );

        if (written == 0) {
            Serial.println("Image upload failed!");
            telegramClient.stop();
            return false;
        }

        sent += written;
    }

    telegramClient.print(tail);

    // Read HTTP status
    unsigned long start = millis();

    while (
        !telegramClient.available() &&
        telegramClient.connected() &&
        millis() - start < 15000
    ) {
        delay(10);
    }

    String response = telegramClient.readStringUntil('\n');

    Serial.print("Telegram response: ");
    Serial.println(response);

    bool success = response.indexOf("200") >= 0;

    telegramClient.stop();

    return success;
}


// SETUP
void setup() {

    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println("ESP32-CAM SECURITY SYSTEM");
    Serial.println("================================");

    // Create mutex
    cameraMutex = xSemaphoreCreateMutex();

    if (cameraMutex == NULL) {
        Serial.println("Cannot create camera mutex!");
        return;
    }

    // PIR setup
    pinMode(PIR_PIN, INPUT);

    // Flash LED setup
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);

    // WiFi setup
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    Serial.println("Connecting to WiFi...");

    WiFi.begin(ssid, password);

    unsigned long wifiStart = millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - wifiStart < 30000
    ) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection failed!");
        return;
    }

    Serial.println("WiFi connected!");

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Initialize camera
    cameraReady = initializeCamera();

    if (!cameraReady) {
        Serial.println("System stopped: camera failed!");
        return;
    }

    delay(1000);

    // Use original CameraWebServer implementation
    startCameraServer();

    Serial.print("Camera stream: http://");
    Serial.println(WiFi.localIP());

    // PIR warmup
    startupTime = millis();

    Serial.println("PIR sensor warming up...");
}


// MAIN LOOP
void loop() {

    if (!cameraReady) {
        delay(1000);
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        return;
    }

    // PIR warmup
    if (millis() - startupTime < PIR_WARMUP_TIME) {
        delay(100);
        return;
    }

    int pirState = digitalRead(PIR_PIN);

    if (pirState == HIGH) {

        if (
            millis() - lastMotionTime <
            MOTION_COOLDOWN
        ) {
            delay(100);
            return;
        }

        lastMotionTime = millis();

        Serial.println("\nMOTION DETECTED!");

        // Bật Flash LED
        digitalWrite(FLASH_LED_PIN, HIGH);
        delay(200);

        // Chụp ảnh
        camera_fb_t* fb = capturePhoto();

        // Tắt Flash LED sau khi chụp
        digitalWrite(FLASH_LED_PIN, LOW);

        if (fb == NULL) {
            Serial.println("Camera capture failed!");
            delay(500);
            return;
        }

        Serial.println("Sending photo to Telegram...");

        bool result = sendPhotoTelegram(fb);

        // Release frame buffer
        esp_camera_fb_return(fb);

        if (result) {
            Serial.println("Telegram photo sent successfully!");
        } else {
            Serial.println("Telegram photo sending failed!");
        }

        Serial.println("Waiting for next motion...");
    }

    delay(100);
}
