typedef struct {
  float topHalfRatio;      // Rasio piksel putih di setengah atas
  float bottomHalfRatio;   // Rasio piksel putih di setengah bawah
  float verticalSpread;    // Sebaran vertikal (tinggi objek)
  int centerOfMassY;       // Pusat massa vertikal (dalam piksel)
  bool isStanding;         // Status: berdiri atau tidak
  float confidence;        // Tingkat kepercayaan (0-100%)
}PostureAnalysis;

PostureAnalysis analyzePosture(uint8_t *data, int width, int height);

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

// Resolusi frame QQVGA
#define FRAME_WIDTH  160
#define FRAME_HEIGHT 120

// ============================================================
// ---------- Otsu Threshold Function ----------
uint8_t calculateOtsuThreshold(uint8_t *data, size_t len) {
  // Buat histogram (256 bins untuk grayscale 0-255)
  uint32_t histogram[256] = {0};
  
  // Hitung histogram
  for (size_t i = 0; i < len; i++) {
    histogram[data[i]]++;
  }
  
  // Total piksel
  uint32_t total = len;
  
  // Hitung sum total (untuk mean)
  float sum = 0;
  for (int i = 0; i < 256; i++) {
    sum += i * histogram[i];
  }
  
  float sumB = 0;    // Sum background
  uint32_t wB = 0;   // Weight background
  uint32_t wF = 0;   // Weight foreground
  float varMax = 0;  // Variance maksimum
  uint8_t threshold = 0;
  
  // Iterasi semua kemungkinan threshold
  for (int t = 0; t < 256; t++) {
    wB += histogram[t];  // Weight background
    if (wB == 0) continue;
    
    wF = total - wB;     // Weight foreground
    if (wF == 0) break;
    
    sumB += (float)(t * histogram[t]);
    
    float mB = sumB / wB;           // Mean background
    float mF = (sum - sumB) / wF;   // Mean foreground
    
    // Hitung between-class variance
    float varBetween = (float)wB * (float)wF * (mB - mF) * (mB - mF);
    
    // Cek apakah ini variance maksimum
    if (varBetween > varMax) {
      varMax = varBetween;
      threshold = t;
    }
  }
  
  return threshold;
}

// ---------- Apply Threshold ----------
void applyThreshold(uint8_t *data, size_t len, uint8_t threshold) {
  for (size_t i = 0; i < len; i++) {
    data[i] = (data[i] >= threshold) ? 255 : 0;
  }
}

// ---------- Analisis Distribusi Vertikal ----------
// typedef struct {
//   float topHalfRatio;      // Rasio piksel putih di setengah atas
//   float bottomHalfRatio;   // Rasio piksel putih di setengah bawah
//   float verticalSpread;    // Sebaran vertikal (tinggi objek)
//   int centerOfMassY;       // Pusat massa vertikal (dalam piksel)
//   bool isStanding;         // Status: berdiri atau tidak
//   float confidence;        // Tingkat kepercayaan (0-100%)
// }PostureAnalysis;

// PostureAnalysis analyzePosture(uint8_t *data, int width, int height);
PostureAnalysis analyzePosture(uint8_t *data, int width, int height) {
  PostureAnalysis result = {0};
  
  // Array untuk menghitung piksel putih per baris
  uint16_t rowWhiteCount[FRAME_HEIGHT] = {0};
  uint32_t totalWhite = 0;
  
  // Hitung piksel putih per baris
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = y * width + x;
      if (data[idx] == 255) {
        rowWhiteCount[y]++;
        totalWhite++;
      }
    }
  }
  
  // Jika tidak ada objek terdeteksi
  if (totalWhite < 100) {
    result.isStanding = false;
    result.confidence = 0;
    return result;
  }
  
  // --- Analisis Setengah Atas vs Bawah ---
  uint32_t topHalfWhite = 0;
  uint32_t bottomHalfWhite = 0;
  int midHeight = height / 2;
  
  for (int y = 0; y < midHeight; y++) {
    topHalfWhite += rowWhiteCount[y];
  }
  for (int y = midHeight; y < height; y++) {
    bottomHalfWhite += rowWhiteCount[y];
  }
  
  result.topHalfRatio = (float)topHalfWhite / totalWhite * 100;
  result.bottomHalfRatio = (float)bottomHalfWhite / totalWhite * 100;
  
  // --- Hitung Center of Mass Vertikal ---
  uint32_t sumY = 0;
  for (int y = 0; y < height; y++) {
    sumY += y * rowWhiteCount[y];
  }
  result.centerOfMassY = sumY / totalWhite;
  
  // --- Hitung Vertical Spread (tinggi objek) ---
  int firstRow = -1, lastRow = -1;
  for (int y = 0; y < height; y++) {
    if (rowWhiteCount[y] > 0) {
      if (firstRow == -1) firstRow = y;
      lastRow = y;
    }
  }
  result.verticalSpread = (float)(lastRow - firstRow) / height * 100;
  
  // --- LOGIKA DETEKSI POSISI ---
  // Berdiri: objek lebih tinggi secara vertikal
  // Horizontal: objek lebih rendah dan tersebar horizontal
  
  float aspectScore = 0;
  
  // 1. Vertical spread tinggi = kemungkinan berdiri
  if (result.verticalSpread > 60) {
    aspectScore += 40;
  } else if (result.verticalSpread > 40) {
    aspectScore += 20;
  }
  
  // 2. Distribusi merata vertikal = berdiri
  float verticalBalance = 100 - abs(result.topHalfRatio - result.bottomHalfRatio);
  if (verticalBalance > 70) {
    aspectScore += 30;
  } else if (verticalBalance > 50) {
    aspectScore += 15;
  }
  
  // 3. Center of mass di tengah atau atas = berdiri
  float centerRatio = (float)result.centerOfMassY / height;
  if (centerRatio > 0.3 && centerRatio < 0.7) {
    aspectScore += 30;
  } else if (centerRatio < 0.3) {
    aspectScore += 20; // Sangat di atas = berdiri
  }
  
  result.confidence = aspectScore;
  result.isStanding = (aspectScore > 50);
  
  return result;
}

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

// ---------- Proses Frame dengan Analisis Postur ----------
void processFrame(camera_fb_t *fb) {
  // Hitung rata-rata intensitas
  uint32_t sum = 0;
  for (size_t i = 0; i < fb->len; i++) {
    sum += fb->buf[i];
  }
  uint8_t avg = sum / fb->len;
  Serial.print("Rata-rata intensitas: ");
  Serial.println(avg);
  
  // Hitung threshold Otsu
  uint8_t otsuThreshold = calculateOtsuThreshold(fb->buf, fb->len);
  Serial.print("Otsu Threshold: ");
  Serial.println(otsuThreshold);
  
  // Terapkan threshold (mengubah gambar menjadi binary)
  applyThreshold(fb->buf, fb->len, otsuThreshold);
  
  // Hitung jumlah piksel putih (foreground)
  uint32_t whitePx = 0;
  for (size_t i = 0; i < fb->len; i++) {
    if (fb->buf[i] == 255) whitePx++;
  }
  float whiteRatio = (float)whitePx / fb->len * 100;
  Serial.print("Piksel putih: ");
  Serial.print(whiteRatio);
  Serial.println("%");
  
  // === ANALISIS POSTUR ===
  Serial.println("\n=== ANALISIS POSTUR ===");
  PostureAnalysis posture = analyzePosture(fb->buf, FRAME_WIDTH, FRAME_HEIGHT);
  
  Serial.print("Distribusi Atas: ");
  Serial.print(posture.topHalfRatio, 1);
  Serial.println("%");
  
  Serial.print("Distribusi Bawah: ");
  Serial.print(posture.bottomHalfRatio, 1);
  Serial.println("%");
  
  Serial.print("Sebaran Vertikal: ");
  Serial.print(posture.verticalSpread, 1);
  Serial.println("%");
  
  Serial.print("Center of Mass Y: ");
  Serial.print(posture.centerOfMassY);
  Serial.print(" px (");
  Serial.print((float)posture.centerOfMassY / FRAME_HEIGHT * 100, 1);
  Serial.println("%)");
  
  Serial.print("\nStatus: ");
  if (posture.isStanding) {
    Serial.println("BERDIRI");
  } else {
    Serial.println("HORIZONTAL/JATUH");
  }
  
  Serial.print("Confidence: ");
  Serial.print(posture.confidence, 1);
  Serial.println("%");
  Serial.println("========================\n");
  
  // Trigger aksi jika terdeteksi jatuh
  if (!posture.isStanding && posture.confidence > 30) {
    Serial.println("⚠️ ALERT: Kemungkinan Tidur Terdeteksi!");
    // Tambahkan aksi: kirim notifikasi, nyalakan alarm, dll
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  
  // esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  // Serial.print("Wakeup cause: ");
  // Serial.println(cause);
  
  pinMode(WAKE_GPIO, INPUT);
  
  if (!initCamera()) {
    Serial.println("Camera gagal init, berhenti");
    while (1) delay(1000);
    // goToDeepSleep(30);
  }
  
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture gagal");
    // goToDeepSleep(30);
  }
  
  processFrame(fb);
  
  esp_camera_fb_return(fb);

  Serial.println("Camera siap. Mulai analisis...");
  
  // Tidur 60 detik atau sampai GPIO aktif
  // goToDeepSleep(60);
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture gagal");
    delay(3000);
    return;
  }

  processFrame(fb);

  esp_camera_fb_return(fb);

  Serial.println("Menunggu 3 detik...\n");
  delay(3000);
}