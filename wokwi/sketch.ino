/*
  Sistem Peringatan Dini Kelembapan dan CO2 Lab Komputer
  Metode : Fuzzy Mamdani (2 input, 9 aturan) + prediksi tren
  Board  : ESP32 DevKit
  Sensor : DHT22 dalam, DHT22 luar (koridor), sensor CO2
           (di Wokwi CO2 diganti potensiometer)
  Output : LED status, buzzer, LCD 16x2 I2C, bot Telegram
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHTesp.h"
#include <math.h>

// ================= PENGATURAN =================
const char* WIFI_SSID = "Wokwi-GUEST";   // ganti dengan WiFi asli saat pakai hardware
const char* WIFI_PASS = "";
const char* BOT_TOKEN = "ISI_TOKEN_BOT";  // dari @BotFather
const char* CHAT_ID   = "ISI_CHAT_ID";    // dari @userinfobot
const char* NAMA_RUANG = "Lab Komputer";

// 0 = simulasi (potensiometer), 1 = sensor MH-Z19B asli (UART2: RX 16, TX 17)
#define PAKAI_MHZ19 0

const int PIN_DHT_DALAM = 15;
const int PIN_DHT_LUAR  = 19;
const int PIN_CO2_POT   = 34;
const int PIN_LED_HIJAU = 25;
const int PIN_LED_KUNING = 26;
const int PIN_LED_MERAH = 27;
const int PIN_BUZZER    = 13;

const unsigned long INTERVAL_BACA   = 2000;     // ms, batas minimum DHT22
const unsigned long ULANG_BAHAYA    = 600000;   // kirim ulang saat masih BAHAYA tiap 10 menit
const unsigned long JEDA_PREDIKSI   = 600000;   // pesan prediksi paling sering tiap 10 menit
const int   MENIT_PREDIKSI = 30;     // prediksi dilaporkan jika BAHAYA dalam 30 menit atau kurang
const float JARAK_HISTERESIS = 3.0;  // status turun hanya jika nilai 3 poin di bawah batas
// ==============================================

DHTesp dhtDalam, dhtLuar;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// FUZZY-BEGIN
// ---------- Fungsi keanggotaan ----------
float trap(float x, float a, float b, float c, float d) {
  if (x < a || x > d) return 0;
  if (x >= b && x <= c) return 1;
  if (x < b) return (x - a) / (b - a);
  return (d - x) / (d - c);
}
float tri(float x, float a, float b, float c) {
  if (x <= a || x >= c) return (x == b) ? 1 : 0;
  if (x == b) return 1;
  if (x < b) return (x - a) / (b - a);
  return (c - x) / (c - b);
}

// Kelembapan: 0 normal, 1 tinggi, 2 sangat tinggi
void muKelembapan(float rh, float mu[3]) {
  mu[0] = trap(rh, 0, 0, 50, 60);
  mu[1] = tri(rh, 50, 65, 80);
  mu[2] = trap(rh, 70, 80, 100, 100);
}
// CO2: 0 baik, 1 sedang, 2 buruk
void muCO2(float ppm, float mu[3]) {
  mu[0] = trap(ppm, 400, 400, 700, 900);
  mu[1] = tri(ppm, 700, 1000, 1300);
  mu[2] = trap(ppm, 1000, 1400, 3000, 3000);
}
// Status: 0 aman, 1 waspada, 2 bahaya
float muStatus(int k, float x) {
  if (k == 0) return trap(x, 0, 0, 25, 45);
  if (k == 1) return tri(x, 30, 50, 70);
  return trap(x, 55, 75, 100, 100);
}

// Basis aturan [kelembapan][co2] -> status
const int ATURAN[3][3] = {
  {0, 0, 1},   // RH normal       : baik, sedang, buruk
  {1, 1, 2},   // RH tinggi
  {2, 2, 2}    // RH sangat tinggi
};

// Mamdani: AND = min, agregasi = max, defuzzifikasi = centroid
float hitungFuzzy(float rh, float ppm) {
  if (rh < 0) rh = 0;
  if (rh > 100) rh = 100;
  if (ppm < 400) ppm = 400;
  if (ppm > 3000) ppm = 3000;

  float mRH[3], mC[3];
  muKelembapan(rh, mRH);
  muCO2(ppm, mC);

  float kekuatan[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      float w = fminf(mRH[i], mC[j]);
      int k = ATURAN[i][j];
      if (w > kekuatan[k]) kekuatan[k] = w;
    }
  }

  float pembilang = 0, penyebut = 0;
  for (int x = 0; x <= 100; x++) {
    float mu = 0;
    for (int k = 0; k < 3; k++) {
      float v = fminf(muStatus(k, x), kekuatan[k]);
      if (v > mu) mu = v;
    }
    pembilang += mu * x;
    penyebut += mu;
  }
  return (penyebut > 0) ? pembilang / penyebut : 0;
}

int labelStatus(float nilai) {
  if (nilai >= 65) return 2;
  if (nilai >= 35) return 1;
  return 0;
}

// Kelembapan absolut (gram uap air per m3)
float kelembapanAbsolut(float suhu, float rh) {
  float es = 6.112f * expf(17.67f * suhu / (suhu + 243.5f));
  return es * rh * 2.1674f / (273.15f + suhu);
}
// FUZZY-END

// ---------- Status dengan histeresis ----------
// Naik langsung. Turun hanya jika nilai sudah JARAK_HISTERESIS di bawah batas,
// supaya status tidak bolak-balik (dan bot tidak spam) saat nilai di sekitar batas.
int statusHisteresis(float nilai, int sebelumnya) {
  int st = labelStatus(nilai);
  if (sebelumnya < 0 || st >= sebelumnya) return st;
  return labelStatus(nilai + JARAK_HISTERESIS);
}

// Penyebab status diambil dari aturan fuzzy yang menghasilkan WASPADA atau BAHAYA:
// kelembapan tinggi/sangat tinggi dan/atau CO2 buruk. Faktor yang jauh lebih lemah
// (kurang dari separuh faktor terkuat) tidak disebut.
void cariPenyebab(int st, float rh, float ppm, bool& masalahRH, bool& masalahCO2) {
  masalahRH = false;
  masalahCO2 = false;
  if (st == 0) return;
  float mRH[3], mC[3];
  muKelembapan(rh, mRH);
  muCO2(ppm, mC);
  float sRH = 0, sCO2 = 0;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      if (ATURAN[i][j] == 0) continue;
      float w = fminf(mRH[i], mC[j]);
      if (i > 0) sRH = fmaxf(sRH, w);
      if (j == 2) sCO2 = fmaxf(sCO2, w);
    }
  }
  float terkuat = fmaxf(sRH, sCO2);
  masalahRH = sRH > 0 && sRH >= 0.5f * terkuat;
  masalahCO2 = sCO2 > 0 && sCO2 >= 0.5f * terkuat;
}

// ---------- Prediksi tren (regresi linear) ----------
const int N_RIWAYAT = 30;   // 30 sampel x 2 detik = 1 menit
float riwayatRH[N_RIWAYAT], riwayatCO2[N_RIWAYAT];
unsigned long riwayatWaktu[N_RIWAYAT];
int jumlahSampel = 0, indeksSampel = 0;

void simpanRiwayat(float rh, float ppm) {
  riwayatRH[indeksSampel] = rh;
  riwayatCO2[indeksSampel] = ppm;
  riwayatWaktu[indeksSampel] = millis();
  indeksSampel = (indeksSampel + 1) % N_RIWAYAT;
  if (jumlahSampel < N_RIWAYAT) jumlahSampel++;
}

// Kemiringan dalam satuan per menit. Memakai waktu baca sebenarnya, karena jarak
// antarsampel bisa lebih dari 2 detik saat program sedang mengirim pesan bot.
float kemiringan(float* data) {
  if (jumlahSampel < 10) return 0;
  int awal = (jumlahSampel < N_RIWAYAT) ? 0 : indeksSampel;
  float sx = 0, sy = 0, sxy = 0, sxx = 0;
  for (int i = 0; i < jumlahSampel; i++) {
    int k = (awal + i) % N_RIWAYAT;
    float x = (riwayatWaktu[k] - riwayatWaktu[awal]) / 60000.0f;
    float y = data[k];
    sx += x; sy += y; sxy += x * y; sxx += x * x;
  }
  float n = jumlahSampel;
  float pembagi = n * sxx - sx * sx;
  if (pembagi == 0) return 0;
  return (n * sxy - sx * sy) / pembagi;
}

// Menit sampai fuzzy masuk BAHAYA jika tren sekarang berlanjut, dicek tiap menit
// sampai MENIT_PREDIKSI. Mengembalikan -1 jika tidak diperkirakan BAHAYA.
// rhNanti dan ppmNanti berisi perkiraan nilai saat BAHAYA, untuk menyusun saran.
int menitKeBahaya(float rh, float ppm, float lajuRH, float lajuCO2,
                  float& rhNanti, float& ppmNanti) {
  if (lajuRH <= 0.01f && lajuCO2 <= 0.01f) return -1;   // tidak ada yang sedang naik
  for (int m = 1; m <= MENIT_PREDIKSI; m++) {
    rhNanti = rh + lajuRH * m;
    ppmNanti = ppm + lajuCO2 * m;
    if (labelStatus(hitungFuzzy(rhNanti, ppmNanti)) == 2) return m;
  }
  return -1;
}

// ---------- Sensor ----------
float bacaCO2() {
#if PAKAI_MHZ19
  const uint8_t perintah[9] = {0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79};
  uint8_t balas[9];
  while (Serial2.available()) Serial2.read();
  Serial2.write(perintah, 9);
  if (Serial2.readBytes(balas, 9) == 9 && balas[0] == 0xFF && balas[1] == 0x86) {
    // Checksum MH-Z19B: 0xFF dikurangi jumlah byte 1 sampai 7, ditambah 1
    uint8_t jumlah = 0;
    for (int i = 1; i < 8; i++) jumlah += balas[i];
    if ((uint8_t)(0xFF - jumlah + 1) == balas[8]) return balas[2] * 256 + balas[3];
  }
  return -1;
#else
  int adc = analogRead(PIN_CO2_POT);           // 0 sampai 4095
  return 400 + (adc / 4095.0f) * 2600;         // 400 sampai 3000 ppm
#endif
}

// ---------- Telegram ----------
String urlEncode(const String& s) {
  String hasil;
  const char* hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      hasil += c;
    } else {
      hasil += '%';
      hasil += hex[(c >> 4) & 0x0F];
      hasil += hex[c & 0x0F];
    }
  }
  return hasil;
}

bool kirimTelegram(const String& pesan) {
  Serial.println("----- PESAN BOT -----");
  Serial.println(pesan);
  Serial.println("---------------------");
  if (String(BOT_TOKEN) == "ISI_TOKEN_BOT") {
    Serial.println("[BOT] Token belum diisi, pesan hanya ditampilkan di Serial.");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BOT] WiFi tidak terhubung.");
    return false;
  }
  WiFiClientSecure klien;
  klien.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN +
               "/sendMessage?chat_id=" + CHAT_ID + "&text=" + urlEncode(pesan);
  http.begin(klien, url);
  int kode = http.GET();
  http.end();
  Serial.printf("[BOT] Kode HTTP: %d\n", kode);
  return kode == 200;
}

const char* NAMA_STATUS[3] = {"AMAN", "WASPADA", "BAHAYA"};
const char* SINGKAT[3] = {"AMN", "WSP", "BHY"};

String buatSaran(bool masalahRH, bool masalahCO2, float ahDalam, float ahLuar) {
  bool luarLebihLembap = ahLuar > ahDalam + 1.0f;
  String s;
  if (isnan(ahLuar) && (masalahRH || masalahCO2)) {
    // Sensor luar gagal: jangan menebak udara luar lebih kering atau lebih lembap
    if (masalahRH && masalahCO2) s = "Buka pintu singkat saja (2 sampai 3 menit) untuk CO2, lalu tutup. Nyalakan AC mode dry.";
    else if (masalahRH) s = "Nyalakan AC mode dry.";
    else s = "Buka pintu singkat untuk sirkulasi udara.";
    s += " Sensor luar tidak terbaca, saran belum membandingkan udara luar.";
  } else if (masalahRH && masalahCO2) {
    if (luarLebihLembap) s = "Buka pintu singkat saja (2 sampai 3 menit) untuk CO2, lalu tutup. Nyalakan AC mode dry.";
    else s = "Buka pintu untuk sirkulasi, udara luar lebih kering. AC mode dry jika ada.";
  } else if (masalahRH) {
    if (luarLebihLembap) s = "Tahan pintu tertutup, udara luar lebih lembap. Nyalakan AC mode dry.";
    else s = "Buka pintu sebentar, udara luar lebih kering. Atau nyalakan AC mode dry.";
  } else if (masalahCO2) {
    if (luarLebihLembap) s = "Buka pintu singkat untuk sirkulasi, jangan terlalu lama karena luar lembap.";
    else s = "Buka pintu untuk sirkulasi udara.";
  } else {
    s = "Tidak perlu tindakan.";
  }
  return s;
}

// masalahRH dan masalahCO2 dari cariPenyebab(). tulisPenyebab false untuk pesan
// prediksi, karena pemicunya sudah dijelaskan di baris tambahan.
String buatPesan(const char* judul, int st, float nilai, float rh, float suhu,
                 float ppm, float rhLuar, float suhuLuar, bool masalahRH, bool masalahCO2,
                 bool tulisPenyebab, const String& tambahan) {
  float ahDalam = kelembapanAbsolut(suhu, rh);
  float ahLuar = kelembapanAbsolut(suhuLuar, rhLuar);
  String p = String("[") + judul + "] " + NAMA_RUANG + "\n";
  p += "Status: " + String(NAMA_STATUS[st]) + " (nilai " + String(nilai, 0) + ")\n";
  p += "Dalam: " + String(rh, 0) + "% , " + String(suhu, 1) + " C\n";
  if (isnan(rhLuar)) p += "Luar : tidak terbaca\n";
  else p += "Luar : " + String(rhLuar, 0) + "% , " + String(suhuLuar, 1) + " C\n";
  p += "CO2  : " + String(ppm, 0) + " ppm\n";
  if (tulisPenyebab) {
    if (masalahRH && masalahCO2) p += "Penyebab: kelembapan dan CO2 tinggi\n";
    else if (masalahRH) p += "Penyebab: kelembapan tinggi, risiko embun dan korosi pada komputer\n";
    else if (masalahCO2) p += "Penyebab: CO2 tinggi, sirkulasi udara kurang\n";
  }
  if (tambahan.length() > 0) p += tambahan + "\n";
  p += "Saran: " + buatSaran(masalahRH, masalahCO2, ahDalam, ahLuar);
  return p;
}

// ---------- Output lokal ----------
void setLED(int st) {
  digitalWrite(PIN_LED_HIJAU, st == 0);
  digitalWrite(PIN_LED_KUNING, st == 1);
  digitalWrite(PIN_LED_MERAH, st == 2);
}

void aturBuzzer(int st) {
  // BAHAYA: bunyi putus-putus 0,5 detik
  static bool nyala = false;
  static unsigned long terakhir = 0;
  if (st != 2) {
    if (nyala) { noTone(PIN_BUZZER); nyala = false; }
    return;
  }
  if (millis() - terakhir >= 500) {
    terakhir = millis();
    nyala = !nyala;
    if (nyala) tone(PIN_BUZZER, 1000);
    else noTone(PIN_BUZZER);
  }
}

void tampilLCD(float rh, float ppm, int st, float rhLuar, float suhuLuar, int prediksi) {
  char baris1[17], baris2[17];
  // Contoh: "H: 55% C 950 AMN", "H:100% C3000 BHY" (selalu 16 karakter)
  snprintf(baris1, sizeof(baris1), "H:%3.0f%% C%4.0f %s", rh, ppm, SINGKAT[st]);
  if (prediksi > 0) snprintf(baris2, sizeof(baris2), "Bahaya ~%2d mnt", prediksi);
  else if (isnan(rhLuar)) snprintf(baris2, sizeof(baris2), "Luar: gagal baca");
  else snprintf(baris2, sizeof(baris2), "Luar H:%2.0f%% %2.0fC", rhLuar, suhuLuar);
  lcd.setCursor(0, 0); lcd.print(baris1); for (int i = strlen(baris1); i < 16; i++) lcd.print(' ');
  lcd.setCursor(0, 1); lcd.print(baris2); for (int i = strlen(baris2); i < 16; i++) lcd.print(' ');
}

// ---------- Program utama ----------
int statusTerakhir = -1;
unsigned long waktuKirimBahaya = 0, waktuKirimPrediksi = 0, waktuBaca = 0;
int stTerakhir = 0;

void setup() {
  Serial.begin(115200);
#if PAKAI_MHZ19
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  Serial2.setTimeout(200);
#endif
  pinMode(PIN_LED_HIJAU, OUTPUT);
  pinMode(PIN_LED_KUNING, OUTPUT);
  pinMode(PIN_LED_MERAH, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  dhtDalam.setup(PIN_DHT_DALAM, DHTesp::DHT22);
  dhtLuar.setup(PIN_DHT_LUAR, DHTesp::DHT22);

  lcd.init();
  lcd.backlight();
  lcd.print("Menghubungkan");
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 15000) delay(250);
  Serial.println(WiFi.status() == WL_CONNECTED ? "WiFi terhubung" : "WiFi gagal, lanjut tanpa bot");
  lcd.clear();
}

void loop() {
  aturBuzzer(stTerakhir);

  if (millis() - waktuBaca < INTERVAL_BACA) return;
  waktuBaca = millis();

  TempAndHumidity dalam = dhtDalam.getTempAndHumidity();
  TempAndHumidity luar = dhtLuar.getTempAndHumidity();
  float ppm = bacaCO2();
  bool gagalDalam = isnan(dalam.humidity);
  bool gagalLuar = isnan(luar.humidity);
  bool gagalCO2 = ppm < 0;
  if (gagalLuar) {
    // Sensor luar hanya untuk saran, jadi sistem tetap jalan tanpa perbandingan luar
    Serial.printf("Sensor luar gagal dibaca (%s), saran tanpa perbandingan luar\n",
                  dhtLuar.getStatusString());
  }
  if (gagalDalam || gagalCO2) {
    // Fuzzy butuh kelembapan dalam dan CO2, tanpa itu status tidak bisa dihitung
    Serial.print("Gagal membaca sensor:");
    if (gagalDalam) Serial.printf(" DHT dalam (%s)", dhtDalam.getStatusString());
    if (gagalCO2) Serial.print(" CO2");
    Serial.println();
    char baris2[17];
    snprintf(baris2, sizeof(baris2), "%s%s", gagalDalam ? "DLM " : "", gagalCO2 ? "CO2" : "");
    lcd.setCursor(0, 0); lcd.print("Sensor gagal:   ");
    lcd.setCursor(0, 1); lcd.print(baris2); for (int i = strlen(baris2); i < 16; i++) lcd.print(' ');
    return;
  }

  float rh = dalam.humidity, suhu = dalam.temperature;
  float rhLuar = luar.humidity, suhuLuar = luar.temperature;

  float nilai = hitungFuzzy(rh, ppm);
  int st = statusHisteresis(nilai, statusTerakhir);

  simpanRiwayat(rh, ppm);
  float lajuRH = kemiringan(riwayatRH);    // % per menit
  float lajuCO2 = kemiringan(riwayatCO2);  // ppm per menit
  float rhNanti = rh, ppmNanti = ppm;
  int prediksi = (st != 2) ? menitKeBahaya(rh, ppm, lajuRH, lajuCO2, rhNanti, ppmNanti) : -1;

  setLED(st);
  tampilLCD(rh, ppm, st, rhLuar, suhuLuar, prediksi);
  Serial.printf("RH %.1f%% T %.1fC | Luar %.1f%% %.1fC | CO2 %.0f | fuzzy %.1f %s | laju RH %.2f/mnt CO2 %.1f/mnt | prediksi %d mnt\n",
                rh, suhu, rhLuar, suhuLuar, ppm, nilai, NAMA_STATUS[st], lajuRH, lajuCO2, prediksi);

  bool masalahRH, masalahCO2;
  cariPenyebab(st, rh, ppm, masalahRH, masalahCO2);

  // 1) Status berubah
  if (st != statusTerakhir) {
    if (statusTerakhir != -1 || st != 0) {
      const char* judul = (st == 2) ? "BAHAYA" : (st == 1) ? "WASPADA" : "PULIH";
      kirimTelegram(buatPesan(judul, st, nilai, rh, suhu, ppm, rhLuar, suhuLuar,
                              masalahRH, masalahCO2, true, ""));
    }
    if (st == 2) waktuKirimBahaya = millis();
    statusTerakhir = st;
  }
  // 2) Masih BAHAYA, ingatkan ulang
  else if (st == 2 && millis() - waktuKirimBahaya >= ULANG_BAHAYA) {
    kirimTelegram(buatPesan("BAHAYA MASIH BERLANGSUNG", st, nilai, rh, suhu, ppm, rhLuar, suhuLuar,
                            masalahRH, masalahCO2, true, ""));
    waktuKirimBahaya = millis();
  }

  // 3) Prediksi akan masuk BAHAYA
  if (prediksi > 0 && (waktuKirimPrediksi == 0 || millis() - waktuKirimPrediksi >= JEDA_PREDIKSI)) {
    bool naikRH = lajuRH > 0.01f, naikCO2 = lajuCO2 > 0.01f;
    const char* pemicu = (naikRH && naikCO2) ? "kelembapan dan CO2" : naikRH ? "kelembapan" : "CO2";
    String info = "Prediksi: masuk zona BAHAYA dalam sekitar " + String((float)prediksi, 0) +
                  " menit jika kenaikan " + pemicu + " berlanjut";
    bool nantiRH, nantiCO2;
    cariPenyebab(2, rhNanti, ppmNanti, nantiRH, nantiCO2);
    kirimTelegram(buatPesan("PERINGATAN DINI", st, nilai, rh, suhu, ppm, rhLuar, suhuLuar,
                            nantiRH, nantiCO2, false, info));
    waktuKirimPrediksi = millis();
  }

  stTerakhir = st;
}
