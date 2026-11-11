// ============================================================================
//  Plant Rail - ESP32-CAM (AI-Thinker) camera node
//
//  A separate board from the main controller. No wires between them - both just
//  join the same Wi-Fi network.
//
//    http://plantcam.local/capture          one JPEG snapshot (SVGA)
//    http://plantcam.local/capture?hq=1     one high-resolution JPEG (UXGA) for AI diagnosis
//    http://plantcam.local/capture?flash=1  switch the on-board LED on while shooting
//    http://plantcam.local:81/stream        MJPEG live stream
//    http://plantcam.local/status           small JSON health report
//
//  Board: "AI Thinker ESP32-CAM"   (needs PSRAM, which this board has)
//
//  Sensors: the normal OV2640 gives JPEG directly. Some cheap boards carry a sensor that cannot
//  output JPEG (OV7670, GC0308, ...; error "JPEG format is not supported on this sensor", 0x106).
//  For those the sketch switches to RGB565 and converts every frame to JPEG itself (see RGB_SIZE).
// ============================================================================
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include "secrets.h"

#define HOSTNAME "plantcam"
#define FLASH_LED_PIN 4   // the very bright white LED on the AI-Thinker board

// AI-Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

static const framesize_t NORMAL_SIZE = FRAMESIZE_SVGA;  // 800x600
static const framesize_t HQ_SIZE = FRAMESIZE_UXGA;      // 1600x1200
// Sensors without JPEG (e.g. GC2145, PID 0x2145) deliver RGB565 frames that are converted here.
// One size for everything, because the frame buffer is allocated once: XGA = 1024x768 (1.5 MB of PSRAM).
// If the camera then fails with "no memory", use FRAMESIZE_SVGA (800x600) or FRAMESIZE_VGA (640x480).
static const framesize_t RGB_SIZE = FRAMESIZE_XGA;

static bool jpegSensor = true;  // false: frames are converted to JPEG in software
static httpd_handle_t httpMain = NULL;
static httpd_handle_t httpStream = NULL;

#define PART_BOUNDARY "plantrailframe"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static bool queryHas(httpd_req_t* req, const char* key) {
  char q[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  char v[8];
  return httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK && v[0] == '1';
}

static void setSize(framesize_t s) {
  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor) sensor->set_framesize(sensor, s);
}

// Gives a JPEG for a frame: the frame itself for JPEG sensors, a converted copy otherwise.
// When *mustFree is true the caller frees *out with free().
static bool frameToJpeg(camera_fb_t* fb, uint8_t** out, size_t* len, bool* mustFree) {
  if (fb->format == PIXFORMAT_JPEG) {
    *out = fb->buf;
    *len = fb->len;
    *mustFree = false;
    return true;
  }
  *mustFree = true;
  return frame2jpg(fb, 85, out, len);
}

static esp_err_t captureHandler(httpd_req_t* req) {
  bool hq = queryHas(req, "hq") && jpegSensor;  // sensors without JPEG are limited to 640x480
  bool flash = queryHas(req, "flash");

  if (hq) setSize(HQ_SIZE);
  if (flash) digitalWrite(FLASH_LED_PIN, HIGH);
  if (hq || flash) {  // let exposure settle, throw away the stale frame
    delay(250);
    camera_fb_t* old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (flash) digitalWrite(FLASH_LED_PIN, LOW);
  if (hq) setSize(NORMAL_SIZE);

  uint8_t* jpg = NULL;
  size_t jlen = 0;
  bool mustFree = false;
  if (!fb || !frameToJpeg(fb, &jpg, &jlen, &mustFree)) {
    if (fb) esp_camera_fb_return(fb);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char*)jpg, jlen);
  if (mustFree) free(jpg);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t streamHandler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part[64];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }
    uint8_t* jpg = NULL;
    size_t jlen = 0;
    bool mustFree = false;
    if (!frameToJpeg(fb, &jpg, &jlen, &mustFree)) { esp_camera_fb_return(fb); res = ESP_FAIL; break; }
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)jlen);
      res = httpd_resp_send_chunk(req, part, hlen);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)jpg, jlen);
    if (mustFree) free(jpg);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;  // client closed the stream
  }
  return res;
}

static esp_err_t statusHandler(httpd_req_t* req) {
  sensor_t* s = esp_camera_sensor_get();
  char body[200];
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"uptime_s\":%lu,\"rssi\":%d,\"ip\":\"%s\",\"psram\":%s,\"sensor_pid\":%d,\"jpeg\":%s}",
           (unsigned long)(millis() / 1000), WiFi.RSSI(), WiFi.localIP().toString().c_str(),
           psramFound() ? "true" : "false", s ? (int)s->id.PID : 0, jpegSensor ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t capture = {"/capture", HTTP_GET, captureHandler, NULL};
  httpd_uri_t status = {"/status", HTTP_GET, statusHandler, NULL};
  if (httpd_start(&httpMain, &config) == ESP_OK) {
    httpd_register_uri_handler(httpMain, &capture);
    httpd_register_uri_handler(httpMain, &status);
  }

  config.server_port = 81;
  config.ctrl_port += 1;  // the second server needs its own control socket
  httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, NULL};
  if (httpd_start(&httpStream, &config) == ESP_OK) {
    httpd_register_uri_handler(httpStream, &stream);
  }
}

static bool startCamera() {
  camera_config_t config;
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
  config.frame_size = NORMAL_SIZE;
  config.jpeg_quality = 12;  // 0-63, lower = better quality
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {  // no PSRAM: small frames in internal RAM
    config.frame_size = FRAMESIZE_VGA;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_ERR_NOT_SUPPORTED) {  // 0x106: this sensor cannot output JPEG (OV7670, GC0308, ...)
    Serial.println("Sensor has no JPEG output: using RGB565 and converting to JPEG here");
    esp_camera_deinit();
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = RGB_SIZE;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    jpegSensor = false;
    err = esp_camera_init(&config);
  }
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    Serial.printf("Camera sensor PID 0x%x, %s\n", s->id.PID, jpegSensor ? "JPEG output" : "RGB565 converted to JPEG");
    s->set_vflip(s, 0);    // set to 1 if the picture is upside down
    s->set_hmirror(s, 0);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nPlant Rail camera node");

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!startCamera()) {
    Serial.println("Restarting in 5 s...");
    delay(5000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);  // keeps the stream smooth
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print('.');
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  startServers();
  Serial.println("Snapshot: http://" + WiFi.localIP().toString() + "/capture");
  Serial.println("Stream:   http://" + WiFi.localIP().toString() + ":81/stream");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {  // simple reconnect
    WiFi.reconnect();
    delay(5000);
  }
  delay(1000);
}
