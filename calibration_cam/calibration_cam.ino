typedef struct {
  float topHalfRatio;      // Rasio piksel putih di setengah atas
  float bottomHalfRatio;   // Rasio piksel putih di setengah bawah
  float verticalSpread;    // Sebaran vertikal (tinggi objek)
  int centerOfMassY;       // Pusat massa vertikal (dalam piksel)
  bool isStanding;         // Status: berdiri atau tidak
  float confidence;        // Tingkat kepercayaan (0-100%)
} PostureAnalysis;

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

// ============== Region of Interest (ROI) ==============
// Persentase yang diabaikan dari setiap sisi
#define ROI_TOP_MARGIN     15  // 15% dari atas
#define ROI_BOTTOM_MARGIN  10  // 10% dari bawah
#define ROI_LEFT_MARGIN    20  // 20% dari kiri
#define ROI_RIGHT_MARGIN   20  // 20% dari kanan

// Hitung batas ROI dalam piksel
#define ROI_Y_START   (FRAME_HEIGHT * ROI_TOP_MARGIN / 100)
#define ROI_Y_END     (FRAME_HEIGHT * (100 - ROI_BOTTOM_MARGIN) / 100)
#define ROI_X_START   (FRAME_WIDTH * ROI_LEFT_MARGIN / 100)
#define ROI_X_END     (FRAME_WIDTH * (100 - ROI_RIGHT_MARGIN) / 100)

#define ROI_WIDTH     (ROI_X_END - ROI_X_START)
#define ROI_HEIGHT    (ROI_Y_END - ROI_Y_START)

// ============== Background Model ==============
static bool bgInitialized = false;
static uint8_t background[FRAME_WIDTH * FRAME_HEIGHT];

// Kecepatan adaptasi background (0–255)
// Semakin kecil = background berubah lebih lambat
#define BG_LEARNING_RATE 20

// ============== Buffer Multi-Frame untuk Tracking ==============
#define FRAME_HISTORY_SIZE 5  // Simpan 5 frame terakhir

typedef struct {
  int centerOfMassY;
  float topHalfRatio;
  float bottomHalfRatio;
  float verticalSpread;
  unsigned long timestamp;
} FrameHistory;

FrameHistory frameBuffer[FRAME_HISTORY_SIZE];
int frameIndex = 0;
int frameCount = 0;

// Status postur
enum PostureState {
  STATE_UNKNOWN,
  STATE_STANDING,
  STATE_TRANSITIONING,
  STATE_SLEEPING
};

PostureState currentState = STATE_UNKNOWN;
unsigned long stateStartTime = 0;

// ============================================================
// ---------- Otsu Threshold Function dengan ROI ----------
uint8_t calculateOtsuThreshold(uint8_t *data, size_t len) {
  // Buat histogram (256 bins untuk grayscale 0-255)
  uint32_t histogram[256] = {0};
  
  // Hitung histogram HANYA dalam ROI
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    for (int x = ROI_X_START; x < ROI_X_END; x++) {
      int idx = y * FRAME_WIDTH + x;
      histogram[data[idx]]++;
    }
  }
  
  // Total piksel dalam ROI
  uint32_t total = ROI_WIDTH * ROI_HEIGHT;
  
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

// ---------- Apply Threshold dengan ROI ----------
void applyThreshold(uint8_t *data, size_t len, uint8_t threshold) {
  // Terapkan threshold HANYA dalam ROI
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    for (int x = ROI_X_START; x < ROI_X_END; x++) {
      int idx = y * FRAME_WIDTH + x;
      data[idx] = (data[idx] >= threshold) ? 255 : 0;
    }
  }
  
  // Set area di luar ROI menjadi hitam (0)
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      // Skip jika dalam ROI
      if (y >= ROI_Y_START && y < ROI_Y_END && 
          x >= ROI_X_START && x < ROI_X_END) {
        continue;
      }
      int idx = y * FRAME_WIDTH + x;
      data[idx] = 0;
    }
  }
}

// ---------- Analisis Distribusi Vertikal dengan ROI ----------
PostureAnalysis analyzePosture(uint8_t *data, int width, int height) {
  PostureAnalysis result = {0};
  
  // Array untuk menghitung piksel putih per baris DALAM ROI
  uint16_t rowWhiteCount[FRAME_HEIGHT] = {0};
  uint32_t totalWhite = 0;
  
  // Hitung piksel putih per baris HANYA dalam ROI
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    for (int x = ROI_X_START; x < ROI_X_END; x++) {
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
  
  // --- Analisis Setengah Atas vs Bawah (dalam ROI) ---
  uint32_t topHalfWhite = 0;
  uint32_t bottomHalfWhite = 0;
  int midHeight = (ROI_Y_START + ROI_Y_END) / 2;
  
  for (int y = ROI_Y_START; y < midHeight; y++) {
    topHalfWhite += rowWhiteCount[y];
  }
  for (int y = midHeight; y < ROI_Y_END; y++) {
    bottomHalfWhite += rowWhiteCount[y];
  }
  
  result.topHalfRatio = (float)topHalfWhite / totalWhite * 100;
  result.bottomHalfRatio = (float)bottomHalfWhite / totalWhite * 100;
  
  // --- Hitung Center of Mass Vertikal (dalam ROI) ---
  uint32_t sumY = 0;
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    sumY += y * rowWhiteCount[y];
  }
  result.centerOfMassY = sumY / totalWhite;
  
  // --- Hitung Vertical Spread (tinggi objek dalam ROI) ---
  int firstRow = -1, lastRow = -1;
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    if (rowWhiteCount[y] > 0) {
      if (firstRow == -1) firstRow = y;
      lastRow = y;
    }
  }
  result.verticalSpread = (float)(lastRow - firstRow) / ROI_HEIGHT * 100;
  
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
  
  // 3. Center of mass di tengah ROI = berdiri
  float centerRatio = (float)(result.centerOfMassY - ROI_Y_START) / ROI_HEIGHT;
  if (centerRatio > 0.3 && centerRatio < 0.7) {
    aspectScore += 30;
  } else if (centerRatio < 0.3) {
    aspectScore += 20; // Sangat di atas = berdiri
  }
  
  result.confidence = aspectScore;
  result.isStanding = (aspectScore > 50);
  
  return result;
}

// ---------- Simpan Frame ke History ----------
void addFrameToHistory(PostureAnalysis posture) {
  frameBuffer[frameIndex].centerOfMassY = posture.centerOfMassY;
  frameBuffer[frameIndex].topHalfRatio = posture.topHalfRatio;
  frameBuffer[frameIndex].bottomHalfRatio = posture.bottomHalfRatio;
  frameBuffer[frameIndex].verticalSpread = posture.verticalSpread;
  frameBuffer[frameIndex].timestamp = millis();
  
  frameIndex = (frameIndex + 1) % FRAME_HISTORY_SIZE;
  if (frameCount < FRAME_HISTORY_SIZE) {
    frameCount++;
  }
}

// ---------- Analisis Tren Multi-Frame ----------
PostureState analyzePostureTrend() {
  if (frameCount < 3) {
    return STATE_UNKNOWN;  // Belum cukup data
  }
  
  // Hitung rata-rata COM Y dari frame terakhir
  float avgCOMY = 0;
  float avgBottomRatio = 0;
  float avgVerticalSpread = 0;
  
  for (int i = 0; i < frameCount; i++) {
    avgCOMY += frameBuffer[i].centerOfMassY;
    avgBottomRatio += frameBuffer[i].bottomHalfRatio;
    avgVerticalSpread += frameBuffer[i].verticalSpread;
  }
  
  avgCOMY /= frameCount;
  avgBottomRatio /= frameCount;
  avgVerticalSpread /= frameCount;
  
  // Normalisasi COM Y ke persentase DALAM ROI
  float avgCOMYPercent = (avgCOMY - ROI_Y_START) / ROI_HEIGHT * 100;
  
  // Deteksi tren pergerakan COM Y
  int trendDown = 0;  // Menghitung berapa kali COM bergerak ke bawah
  for (int i = 1; i < frameCount; i++) {
    int prevIdx = (frameIndex - frameCount + i - 1 + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE;
    int currIdx = (frameIndex - frameCount + i + FRAME_HISTORY_SIZE) % FRAME_HISTORY_SIZE;
    
    if (frameBuffer[currIdx].centerOfMassY > frameBuffer[prevIdx].centerOfMassY) {
      trendDown++;
    }
  }
  
  Serial.println("\n--- ANALISIS TREN MULTI-FRAME ---");
  Serial.print("Frame yang dianalisis: ");
  Serial.println(frameCount);
  Serial.print("Avg COM Y: ");
  Serial.print(avgCOMY, 1);
  Serial.print(" px (");
  Serial.print(avgCOMYPercent, 1);
  Serial.println("%)");
  Serial.print("Avg Bottom Ratio: ");
  Serial.print(avgBottomRatio, 1);
  Serial.println("%");
  Serial.print("Avg Vertical Spread: ");
  Serial.print(avgVerticalSpread, 1);
  Serial.println("%");
  Serial.print("Tren bergerak ke bawah: ");
  Serial.print(trendDown);
  Serial.print("/");
  Serial.println(frameCount - 1);
  
  // === LOGIKA STATE MACHINE ===
  
  // KONDISI TIDUR/HORIZONTAL:
  // 1. COM Y di bawah 50% (lebih ke bawah frame)
  // 2. Bottom ratio tinggi (>65%)
  // 3. Vertical spread rendah (<35%)
  // 4. Tren bergerak ke bawah (mayoritas frame)
  
  if (avgCOMYPercent > 55 && avgBottomRatio > 65 && avgVerticalSpread < 35) {
    Serial.println("→ POLA TIDUR TERDETEKSI");
    return STATE_SLEEPING;
  }
  
  // KONDISI BERDIRI:
  // 1. COM Y di tengah (30-70%)
  // 2. Vertical spread tinggi (>50%)
  // 3. Distribusi merata
  
  if (avgCOMYPercent >= 30 && avgCOMYPercent <= 70 && avgVerticalSpread > 50) {
    Serial.println("→ POLA BERDIRI TERDETEKSI");
    return STATE_STANDING;
  }
  
  // KONDISI TRANSISI:
  // Ada perubahan signifikan dalam beberapa frame terakhir
  
  if (trendDown >= (frameCount - 1) / 2) {
    Serial.println("→ TRANSISI KE POSISI HORIZONTAL");
    return STATE_TRANSITIONING;
  }
  
  Serial.println("→ STATUS TIDAK JELAS");
  return STATE_UNKNOWN;
}

// ---------- Update State Machine ----------
void updatePostureState(PostureState newState) {
  if (newState != currentState) {
    currentState = newState;
    stateStartTime = millis();
    
    Serial.print("\n🔄 PERUBAHAN STATE: ");
    switch (currentState) {
      case STATE_STANDING:
        Serial.println("BERDIRI");
        break;
      case STATE_TRANSITIONING:
        Serial.println("TRANSISI");
        break;
      case STATE_SLEEPING:
        Serial.println("TIDUR/HORIZONTAL");
        Serial.println("⚠️⚠️⚠️ ALERT: STATUS TIDUR TERDETEKSI! ⚠️⚠️⚠️");
        // Trigger aksi: kirim notifikasi, nyalakan alarm, dll
        break;
      case STATE_UNKNOWN:
        Serial.println("TIDAK DIKETAHUI");
        break;
    }
  }
  
  // Hitung durasi state saat ini
  unsigned long stateDuration = (millis() - stateStartTime) / 1000;
  Serial.print("Durasi state saat ini: ");
  Serial.print(stateDuration);
  Serial.println(" detik");
  
  // Alert jika tidur lebih dari 15 detik
  if (currentState == STATE_SLEEPING && stateDuration > 15) {
    Serial.println("🚨🚨🚨 PERINGATAN: Tidur >15 detik! 🚨🚨🚨");
  }
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

void applyBackgroundSubtraction(uint8_t *frame) {
  // Inisialisasi background dari frame pertama
  if (!bgInitialized) {
    memcpy(background, frame, FRAME_WIDTH * FRAME_HEIGHT);
    bgInitialized = true;
    return;
  }

  // Update background + hitung foreground
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    for (int x = ROI_X_START; x < ROI_X_END; x++) {
      int idx = y * FRAME_WIDTH + x;

      uint8_t curr = frame[idx];
      uint8_t bg   = background[idx];

      // Background update (running average integer)
      background[idx] = bg + ((curr - bg) / BG_LEARNING_RATE);

      // Foreground = selisih absolut
      frame[idx] = abs(curr - bg);
    }
  }

  // Area luar ROI dimatikan
  for (int y = 0; y < FRAME_HEIGHT; y++) {
    for (int x = 0; x < FRAME_WIDTH; x++) {
      if (y >= ROI_Y_START && y < ROI_Y_END &&
          x >= ROI_X_START && x < ROI_X_END) continue;

      frame[y * FRAME_WIDTH + x] = 0;
    }
  }
}

// ---------- Proses Frame dengan Analisis Postur ----------
void processFrame(camera_fb_t *fb) {
  // INFO ROI
  Serial.println("\n========== INFO ROI ==========");
  Serial.print("Frame Size: ");
  Serial.print(FRAME_WIDTH);
  Serial.print("x");
  Serial.println(FRAME_HEIGHT);
  Serial.print("ROI Area: X[");
  Serial.print(ROI_X_START);
  Serial.print("-");
  Serial.print(ROI_X_END);
  Serial.print("] Y[");
  Serial.print(ROI_Y_START);
  Serial.print("-");
  Serial.print(ROI_Y_END);
  Serial.println("]");
  Serial.print("ROI Size: ");
  Serial.print(ROI_WIDTH);
  Serial.print("x");
  Serial.println(ROI_HEIGHT);
  Serial.println("==============================\n");
  
  // Hitung rata-rata intensitas (full frame)
  uint32_t sum = 0;
  for (size_t i = 0; i < fb->len; i++) {
    sum += fb->buf[i];
  }
  uint8_t avg = sum / fb->len;
  Serial.print("Rata-rata intensitas (full frame): ");
  Serial.println(avg);
  
  // === BACKGROUND SUBTRACTION ===
  applyBackgroundSubtraction(fb->buf);
  // Hitung threshold Otsu (hanya ROI)
  uint8_t otsuThreshold = calculateOtsuThreshold(fb->buf, fb->len);
  Serial.print("Otsu Threshold (ROI): ");
  Serial.println(otsuThreshold);
  
  // Terapkan threshold (ROI + blackout area luar)
  applyThreshold(fb->buf, fb->len, otsuThreshold); 
  
  // Hitung jumlah piksel putih (hanya dalam ROI)
  uint32_t whitePx = 0;
  for (int y = ROI_Y_START; y < ROI_Y_END; y++) {
    for (int x = ROI_X_START; x < ROI_X_END; x++) {
      int idx = y * FRAME_WIDTH + x;
      if (fb->buf[idx] == 255) whitePx++;
    }
  }
  float whiteRatio = (float)whitePx / (ROI_WIDTH * ROI_HEIGHT) * 100;
  Serial.print("Piksel putih (ROI): ");
  Serial.print(whiteRatio);
  Serial.println("%");
  
  // === ANALISIS POSTUR SINGLE FRAME ===
  Serial.println("\n=== ANALISIS POSTUR (Frame Saat Ini) ===");
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
  Serial.print((float)(posture.centerOfMassY - ROI_Y_START) / ROI_HEIGHT * 100, 1);
  Serial.println("% dalam ROI)");
  
  Serial.print("Status Frame: ");
  if (posture.isStanding) {
    Serial.println("BERDIRI");
  } else {
    Serial.println("HORIZONTAL");
  }
  
  Serial.print("Confidence: ");
  Serial.print(posture.confidence, 1);
  Serial.println("%");
  
  // Simpan ke history
  addFrameToHistory(posture);
  
  // Analisis tren multi-frame
  PostureState newState = analyzePostureTrend();
  
  // Update state machine
  updatePostureState(newState);
  
  Serial.println("==========================================\n");
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  
  pinMode(WAKE_GPIO, INPUT);
  
  if (!initCamera()) {
    Serial.println("Camera gagal init, berhenti");
    while (1) delay(1000);
  }
  
  Serial.println("Camera siap. Mulai analisis multi-frame...");
  Serial.println("Mengumpulkan 3-5 frame untuk analisis tren...\n");
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