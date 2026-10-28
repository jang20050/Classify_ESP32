/*
 * XIAO ESP32-S3 Sense - camera server for data collection
 *
 *   http://<ip>/capture      single JPEG frame
 *   http://<ip>/status       camera settings as JSON
 *   http://<ip>/control?var=<name>&val=<int>   change a sensor setting
 *   http://<ip>:81/stream    MJPEG live stream
 *
 * Arduino IDE: Board "XIAO_ESP32S3", Tools > PSRAM > "OPI PSRAM" (required)
 * arduino-cli: --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi
 *
 * Put your Wi-Fi credentials in secrets.h
 */
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "secrets.h"

#define HOSTNAME "esp32cam"  // -> http://esp32cam.local

// XIAO ESP32-S3 Sense camera pins
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t camera_httpd = NULL;
static httpd_handle_t stream_httpd = NULL;

static bool initCamera() {
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
  config.frame_size = FRAMESIZE_VGA;  // 640x480, changeable from the dashboard
  config.jpeg_quality = 12;           // 0-63, lower = better quality
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    Serial.println("[WARN] PSRAM not found - enable Tools > PSRAM > OPI PSRAM");
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERR] Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("Camera sensor PID: 0x%x\n", s->id.PID);
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  return true;
}

static void setCommonHeaders(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  setCommonHeaders(req);
  httpd_resp_set_type(req, "image/jpeg");
  char size[32];
  snprintf(size, sizeof(size), "%ux%u", fb->width, fb->height);
  httpd_resp_set_hdr(req, "X-Frame-Size", size);
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t streamHandler(httpd_req_t *req) {
  char part[64];
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  setCommonHeaders(req);

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[ERR] Frame capture failed");
      res = ESP_FAIL;
      break;
    }
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;  // client disconnected
  }
  return res;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  char json[512];
  snprintf(json, sizeof(json),
           "{\"pid\":%u,\"framesize\":%u,\"quality\":%u,\"brightness\":%d,"
           "\"contrast\":%d,\"saturation\":%d,\"special_effect\":%u,"
           "\"awb\":%u,\"aec\":%u,\"agc\":%u,\"hmirror\":%u,\"vflip\":%u,"
           "\"rssi\":%d,\"ip\":\"%s\",\"psram\":%s}",
           s->id.PID, s->status.framesize, s->status.quality, s->status.brightness,
           s->status.contrast, s->status.saturation, s->status.special_effect,
           s->status.awb, s->status.aec, s->status.agc, s->status.hmirror, s->status.vflip,
           WiFi.RSSI(), WiFi.localIP().toString().c_str(), psramFound() ? "true" : "false");
  setCommonHeaders(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char query[96], var[32], val[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need var & val");
    return ESP_FAIL;
  }
  int v = atoi(val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(var, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)v);
  }
  else if (!strcmp(var, "quality")) res = s->set_quality(s, v);
  else if (!strcmp(var, "brightness")) res = s->set_brightness(s, v);
  else if (!strcmp(var, "contrast")) res = s->set_contrast(s, v);
  else if (!strcmp(var, "saturation")) res = s->set_saturation(s, v);
  else if (!strcmp(var, "special_effect")) res = s->set_special_effect(s, v);
  else if (!strcmp(var, "awb")) res = s->set_whitebal(s, v);
  else if (!strcmp(var, "aec")) res = s->set_exposure_ctrl(s, v);
  else if (!strcmp(var, "agc")) res = s->set_gain_ctrl(s, v);
  else if (!strcmp(var, "hmirror")) res = s->set_hmirror(s, v);
  else if (!strcmp(var, "vflip")) res = s->set_vflip(s, v);
  else res = -1;

  setCommonHeaders(req);
  if (res < 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown var or bad value");
    return ESP_FAIL;
  }
  return httpd_resp_send(req, "OK", 2);
}

static void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;

  httpd_uri_t capture_uri = { "/capture", HTTP_GET, captureHandler, NULL };
  httpd_uri_t status_uri = { "/status", HTTP_GET, statusHandler, NULL };
  httpd_uri_t control_uri = { "/control", HTTP_GET, controlHandler, NULL };
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, streamHandler, NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
  }

  // Stream runs on its own server (port 81) so /capture keeps working while streaming
  config.server_port += 1;
  config.ctrl_port += 1;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!initCamera()) {
    delay(3000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  startServers();
  Serial.printf("Ready!  http://%s  (or http://%s.local)\n",
                WiFi.localIP().toString().c_str(), HOSTNAME);
  Serial.printf("Stream: http://%s:81/stream\n", WiFi.localIP().toString().c_str());
}

void loop() {
  // Reconnect if Wi-Fi drops
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost, reconnecting...");
    WiFi.reconnect();
    delay(5000);
  }
  delay(1000);
}
