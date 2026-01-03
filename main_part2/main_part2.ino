#include "DFRobotDFPlayerMini.h"

HardwareSerial dfSerial(2); // UART2
DFRobotDFPlayerMini dfplayer;

const int vibrationPin = 25;   // GPIO25 untuk motor getar

int playCount = 0;
const int maxPlay = 5;
bool isPlaying = false;

void softStartVolume(uint8_t targetVolume) {
  dfplayer.volume(0);
  delay(200);

  for (int v = 0; v <= targetVolume; v++) {
    dfplayer.volume(v);
    delay(60);
  }
}

void startVibration() {
  digitalWrite(vibrationPin, HIGH);
}

void stopVibration() {
  digitalWrite(vibrationPin, LOW);
}

void setup() {
  Serial.begin(115200);

  pinMode(vibrationPin, OUTPUT);
  stopVibration();  // pastikan mati saat awal

  // RX = GPIO16, TX = GPIO17
  dfSerial.begin(9600, SERIAL_8N1, 16, 17);

  Serial.println("Inisialisasi DFPlayer...");

  if (!dfplayer.begin(dfSerial)) {
    Serial.println("❌ DFPlayer tidak terdeteksi");
    while (true);
  }

  Serial.println("✅ DFPlayer siap");

  softStartVolume(30);
  dfplayer.play(3);
  isPlaying = true;
  startVibration();   // getaran mulai
}

void loop() {
  if (dfplayer.available()) {
    uint8_t type = dfplayer.readType();

    if (type == DFPlayerPlayFinished) {
      playCount++;
      Serial.print("Selesai putar ke-");
      Serial.println(playCount);

      if (playCount < maxPlay) {
        delay(500);
        dfplayer.play(3);
        startVibration();   // getaran aktif lagi untuk track berikutnya
      } else {
        Serial.println("🎵 Pemutaran selesai (5 kali)");
        isPlaying = false;
        stopVibration();    // getaran mati total
      }
    }
  }
}
