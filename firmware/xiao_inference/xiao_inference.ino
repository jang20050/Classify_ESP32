/*
 * XIAO ESP32-S3 Sense - on-device Edge Impulse image classification + web dashboard
 *
 *   http://<ip>/            dashboard (live stream + predictions)
 *   http://<ip>/result      latest prediction as JSON
 *   http://<ip>/input.bmp   the 96x96 image the model actually sees
 *   http://<ip>/control?var=<vflip|hmirror|brightness|threshold>&val=<n>
 *   http://<ip>:81/stream   MJPEG live stream
 *
 * Requires the Edge Impulse Arduino library (ei_model/ei-arduino-library.zip)
 * Arduino IDE: Board "XIAO_ESP32S3", Tools > PSRAM > "OPI PSRAM" (required)
 * Put your Wi-Fi credentials in secrets.h
 */
#include <jang20050-project-1_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include "secrets.h"
#include "dashboard.h"

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

// QVGA keeps JPEG decoding fast; the center square (240x240) is resized to the model input
#define FRAME_SIZE   FRAMESIZE_QVGA
#define FRAME_W      320
#define FRAME_H      240
#define IN_W         EI_CLASSIFIER_INPUT_WIDTH
#define IN_H         EI_CLASSIFIER_INPUT_HEIGHT

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t camera_httpd = NULL;
static httpd_handle_t stream_httpd = NULL;

static uint8_t *snapshot_buf = NULL;  // FRAME_W*FRAME_H*3, BGR888 (esp32-camera outputs BGR)

// Latest result, shared between the inference loop and HTTP handlers
static SemaphoreHandle_t result_mutex;
static struct {
  float scores[EI_CLASSIFIER_LABEL_COUNT];
  int top;
  int dsp_ms, cls_ms;
  float fps;
  uint32_t count;
  uint8_t input[IN_W * IN_H * 3];  // BGR, model input
} latest = {};
static float threshold = 0.6f;

// ---------- camera ----------
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
  config.frame_size = FRAME_SIZE;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERR] Camera init failed: 0x%x\n", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("Camera sensor PID: 0x%x\n", s->id.PID);
  // Same orientation/tuning as the data-collection firmware, so inputs match the training images
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  return true;
}

// ---------- inference ----------
static int getSignalData(size_t offset, size_t length, float *out_ptr) {
  size_t px = offset * 3;
  for (size_t i = 0; i < length; i++, px += 3) {
    // BGR -> packed RGB
    out_ptr[i] = (snapshot_buf[px + 2] << 16) + (snapshot_buf[px + 1] << 8) + snapshot_buf[px];
  }
  return 0;
}

static void runInference() {
  static uint32_t last_ms = 0;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERR] Camera capture failed");
    delay(100);
    return;
  }
  int w = fb->width, h = fb->height;
  bool ok = (w * h <= FRAME_W * FRAME_H) && fmt2rgb888(fb->buf, fb->len, fb->format, snapshot_buf);
  esp_camera_fb_return(fb);
  if (!ok) {
    Serial.println("[ERR] JPEG decode failed");
    return;
  }

  // Center-crop to square and resize to the model input (same as "fit-short" in the impulse)
  ei::image::processing::crop_and_interpolate_rgb888(snapshot_buf, w, h, snapshot_buf, IN_W, IN_H);

  signal_t signal;
  signal.total_length = IN_W * IN_H;
  signal.get_data = &getSignalData;
  ei_impulse_result_t result = {};
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    Serial.printf("[ERR] run_classifier failed (%d)\n", err);
    return;
  }

  uint32_t now = millis();
  int top = 0;
  for (int i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > result.classification[top].value) top = i;
  }

  xSemaphoreTake(result_mutex, portMAX_DELAY);
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) latest.scores[i] = result.classification[i].value;
  latest.top = top;
  latest.dsp_ms = result.timing.dsp;
  latest.cls_ms = result.timing.classification;
  if (last_ms) latest.fps = 0.8f * latest.fps + 0.2f * (1000.0f / (now - last_ms));
  latest.count++;
  memcpy(latest.input, snapshot_buf, sizeof(latest.input));
  xSemaphoreGive(result_mutex);
  last_ms = now;

  if (latest.count % 10 == 0) {
    Serial.printf("%s %.2f  (dsp %d ms, nn %d ms, %.1f fps)\n", ei_classifier_inferencing_categories[top],
                  result.classification[top].value, result.timing.dsp, result.timing.classification, latest.fps);
  }
}

// ---------- HTTP ----------
static void setCommonHeaders(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, DASHBOARD_HTML, strlen(DASHBOARD_HTML));
}

static esp_err_t resultHandler(httpd_req_t *req) {
  char json[160 + EI_CLASSIFIER_LABEL_COUNT * 64];
  int n = snprintf(json, sizeof(json), "{\"labels\":[");
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    n += snprintf(json + n, sizeof(json) - n, "%s\"%s\"", i ? "," : "", ei_classifier_inferencing_categories[i]);
  }
  n += snprintf(json + n, sizeof(json) - n, "],\"scores\":[");
  xSemaphoreTake(result_mutex, portMAX_DELAY);
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    n += snprintf(json + n, sizeof(json) - n, "%s%.4f", i ? "," : "", latest.scores[i]);
  }
  n += snprintf(json + n, sizeof(json) - n,
                "],\"top\":%d,\"dsp_ms\":%d,\"nn_ms\":%d,\"fps\":%.2f,\"count\":%lu,"
                "\"threshold\":%.2f,\"rssi\":%d,\"input\":[%d,%d]}",
                latest.top, latest.dsp_ms, latest.cls_ms, latest.fps, (unsigned long)latest.count,
                threshold, WiFi.RSSI(), IN_W, IN_H);
  xSemaphoreGive(result_mutex);
  setCommonHeaders(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, n);
}

// 24-bit BMP of the model input (BMP stores BGR bottom-up, which matches our buffer order)
static esp_err_t inputBmpHandler(httpd_req_t *req) {
  const uint32_t row = IN_W * 3;  // 288, already a multiple of 4
  const uint32_t data_size = row * IN_H, file_size = 54 + data_size;
  static uint8_t bmp[54 + IN_W * IN_H * 3];
  uint8_t hdr[54] = { 'B', 'M' };
  auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
  put32(2, file_size);
  put32(10, 54);
  put32(14, 40);
  put32(18, IN_W);
  put32(22, IN_H);
  hdr[26] = 1;
  hdr[28] = 24;
  put32(34, data_size);
  memcpy(bmp, hdr, 54);
  xSemaphoreTake(result_mutex, portMAX_DELAY);
  for (int y = 0; y < IN_H; y++) memcpy(bmp + 54 + y * row, latest.input + (IN_H - 1 - y) * row, row);
  xSemaphoreGive(result_mutex);
  setCommonHeaders(req);
  httpd_resp_set_type(req, "image/bmp");
  return httpd_resp_send(req, (const char *)bmp, file_size);
}

static esp_err_t controlHandler(httpd_req_t *req) {
  char query[96], var[32], val[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need var & val");
    return ESP_FAIL;
  }
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;
  if (!strcmp(var, "threshold")) threshold = constrain(atof(val), 0.0f, 1.0f);
  else if (!strcmp(var, "vflip")) res = s->set_vflip(s, atoi(val));
  else if (!strcmp(var, "hmirror")) res = s->set_hmirror(s, atoi(val));
  else if (!strcmp(var, "brightness")) res = s->set_brightness(s, atoi(val));
  else res = -1;

  setCommonHeaders(req);
  if (res < 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown var or bad value");
    return ESP_FAIL;
  }
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t statusHandler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  char json[128];
  snprintf(json, sizeof(json), "{\"pid\":%u,\"vflip\":%u,\"hmirror\":%u,\"brightness\":%d}",
           s->id.PID, s->status.vflip, s->status.hmirror, s->status.brightness);
  setCommonHeaders(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t streamHandler(httpd_req_t *req) {
  char part[64];
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  setCommonHeaders(req);

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;  // client disconnected
    delay(30);                 // leave CPU time for inference
  }
  return res;
}

static void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri = { "/", HTTP_GET, indexHandler, NULL };
  httpd_uri_t result_uri = { "/result", HTTP_GET, resultHandler, NULL };
  httpd_uri_t input_uri = { "/input.bmp", HTTP_GET, inputBmpHandler, NULL };
  httpd_uri_t control_uri = { "/control", HTTP_GET, controlHandler, NULL };
  httpd_uri_t status_uri = { "/status", HTTP_GET, statusHandler, NULL };
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, streamHandler, NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &result_uri);
    httpd_register_uri_handler(camera_httpd, &input_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
  }

  config.server_port += 1;
  config.ctrl_port += 1;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  result_mutex = xSemaphoreCreateMutex();
  snapshot_buf = (uint8_t *)ps_malloc(FRAME_W * FRAME_H * 3);
  if (!psramFound() || !snapshot_buf) {
    Serial.println("[ERR] PSRAM required - enable Tools > PSRAM > OPI PSRAM");
    while (true) delay(1000);
  }
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

  Serial.printf("Model: %d labels, input %dx%d\n", EI_CLASSIFIER_LABEL_COUNT, IN_W, IN_H);
  Serial.printf("Dashboard: http://%s  (or http://%s.local)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
}

void loop() {
  runInference();
}
