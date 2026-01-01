#include "DFRobotDFPlayerMini.h"

HardwareSerial dfSerial(2); // UART2
DFRobotDFPlayerMini dfplayer;

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

void setup() {
  Serial.begin(115200);

  // RX = GPIO16, TX = GPIO17
  dfSerial.begin(9600, SERIAL_8N1, 16, 17);

  Serial.println("Inisialisasi DFPlayer...");

  if (!dfplayer.begin(dfSerial)) {
    Serial.println("❌ DFPlayer tidak terdeteksi");
    while (true);
  }

  Serial.println("✅ DFPlayer siap");

  softStartVolume(30);  // atau 18–22
  dfplayer.play(3);      // mulai putaran pertama
  isPlaying = true;
}


void loop() {
  if (dfplayer.available()) {
    uint8_t type = dfplayer.readType();

    if (type == DFPlayerPlayFinished) {
      playCount++;
      Serial.print("Selesai putar ke-");
      Serial.println(playCount);

      if (playCount < maxPlay) {
        delay(500);          // jeda kecil (opsional)
        dfplayer.play(3);    // putar lagi
      } else {
        Serial.println("🎵 Pemutaran selesai (5 kali)");
        isPlaying = false;
      }
    }
  }
}