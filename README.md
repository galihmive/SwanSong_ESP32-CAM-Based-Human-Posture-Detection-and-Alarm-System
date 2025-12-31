# SwanSong_ESP32-CAM-Based-Human-Posture-Detection-and-Alarm-System
SwanSong adalah proyek alarm pintar berbasis ESP32-CAM yang dirancang untuk memastikan pengguna benar-benar bangun dari tidur. Berbeda dari alarm konvensional yang dapat dimatikan secara manual, SwanSong hanya akan berhenti ketika sistem mendeteksi bahwa pengguna telah berdiri di depan kamera.

Inti sistem ini adalah pemrosesan citra sederhana pada perangkat dengan sumber daya terbatas. Kamera ESP32-CAM menangkap citra pengguna, lalu sistem menganalisis distribusi piksel untuk memperkirakan postur tubuh. Jika postur yang terdeteksi memenuhi kriteria “berdiri”, maka alarm akan dimatikan secara otomatis. Jika tidak, alarm akan terus aktif hingga kondisi terpenuhi atau batas waktu tertentu tercapai.
