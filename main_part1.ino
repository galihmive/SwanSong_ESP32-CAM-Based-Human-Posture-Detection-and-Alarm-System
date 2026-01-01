#include "esp_camera.h"
#include "esp_sleep.h"
// ================= PIN ESP32-CAM AI Thinker =================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
// GPIO untuk wake-up eksternal (contoh: sensor getar / PIR)
#define WAKE_GPIO GPIO_NUM_13
// ============================================================
// ---------- Deep Sleep Function ----------
void goToDeepSleep(uint64_t sleep_seconds) {
  Serial.println("Masuk deep sleep...");
  esp_camera_deinit();   // WAJIB agar hemat daya
  esp_sleep_enable_timer_wakeup(sleep_seconds * 1000000ULL);
  esp_sleep_enable_ext0_wakeup(WAKE_GPIO, 1); // HIGH untuk bangun
  delay(100);
  esp_deep_sleep_start();
}
// ---------- Kamera Init ----------
bool initCamera() {
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
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size   = FRAMESIZE_QQVGA;  // 160x120
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init gagal");
    return false;
  }
  return true;
}
// ---------- Contoh Proses Frame ----------
void processFrame(camera_fb_t *fb) {
  // fb->buf  : data grayscale 8-bit
  // fb->len  : ukuran buffer
  uint32_t sum = 0;
  for (size_t i = 0; i < fb->len; i++) {
    sum += fb->buf[i];
  }
  uint8_t avg = sum / fb->len;
  Serial.print("Rata-rata intensitas: ");
  Serial.println(avg);
  // Pengembangan nanti:
  // - threshold
  // - deteksi siluet
  // - cek posisi berdiri
}
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.print("Wakeup cause: ");
  Serial.println(cause);
  pinMode(WAKE_GPIO, INPUT);
  if (!initCamera()) {
    goToDeepSleep(30);
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture gagal");
    goToDeepSleep(30);
  }
  processFrame(fb);
  esp_camera_fb_return(fb);
  // Tidur 60 detik atau sampai GPIO aktif
  goToDeepSleep(60);
}
void loop() {
  // Tidak dipakai
}