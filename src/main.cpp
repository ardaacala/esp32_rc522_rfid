#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <cstring>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define RFID_WIFI_SSID ""
#define RFID_WIFI_PASSWORD ""
#define RFID_OTA_PASSWORD ""
#endif

// USB and the single TCP client share the same commands and output.
class ConsolePort : public Stream {
 public:
  using Print::write;
  void begin(unsigned long baud) { ::Serial.begin(baud); }
  void setClient(WiFiClient client) { client_ = client; }
  void dropClient() { client_.stop(); }
  bool connected() { return client_ && client_.connected(); }
  int available() override {
    if (::Serial.available()) return ::Serial.available();
    return connected() ? client_.available() : 0;
  }
  int read() override {
    if (::Serial.available()) return ::Serial.read();
    return connected() ? client_.read() : -1;
  }
  int peek() override {
    if (::Serial.available()) return ::Serial.peek();
    return connected() ? client_.peek() : -1;
  }
  void flush() override { ::Serial.flush(); }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    const size_t sent = ::Serial.write(data, length);
    if (connected() && client_.availableForWrite() >= static_cast<int>(length)) {
      client_.write(data, length);
    }
    return sent;
  }
 private:
  WiFiClient client_;
};

ConsolePort console;

namespace {
constexpr uint8_t PIN_SS = 21;
constexpr uint8_t PIN_RST = 22;
constexpr uint8_t DATA_BLOCKS[] = {4, 5, 6};
constexpr size_t RECORD_SIZE = 48;
constexpr size_t MAX_TEXT_BYTES = 43;
constexpr size_t COMMAND_BUFFER_SIZE = 128;
constexpr unsigned long WAIT_TIMEOUT_MS = 30000;
constexpr unsigned long WRITE_WAIT_TIMEOUT_MS = 5000;
constexpr unsigned long CARD_POLL_MS = 50;
constexpr unsigned long READER_PROBE_MS = 1000;
constexpr unsigned long SAME_CARD_GAP_MS = 700;
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr uint16_t CONSOLE_PORT = 2323;

MFRC522 reader(PIN_SS, PIN_RST);
WiFiServer networkConsole(CONSOLE_PORT);
MFRC522::MIFARE_Key defaultKey = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

enum class PendingOperation { None, Info, Read, Write };
PendingOperation pending = PendingOperation::None;
unsigned long pendingSince = 0;
unsigned long lastCardPoll = 0;
unsigned long lastScanErrorAt = 0;
unsigned long lastReaderProbe = 0;
bool readerReady = false;
char commandBuffer[COMMAND_BUFFER_SIZE];
size_t commandLength = 0;
bool commandOverflow = false;
uint8_t textToWrite[MAX_TEXT_BYTES];
uint8_t textLength = 0;
uint16_t writeTarget = 1;
uint16_t writeCompleted = 0;
uint8_t lastWrittenText[MAX_TEXT_BYTES];
uint8_t lastWrittenLength = 0;
bool hasLastWrittenText = false;
unsigned long lastWifiAttempt = 0;
bool networkStarted = false;
bool waitForCardRemoval = false;
uint8_t lastUid[10];
byte lastUidSize = 0;
unsigned long lastSeenAt = 0;

void printStatus(const char *action, MFRC522::StatusCode status) {
  console.print(action);
  console.print(F(": "));
  switch (status) {
    case MFRC522::STATUS_ERROR: console.println(F("iletişim hatası")); break;
    case MFRC522::STATUS_COLLISION: console.println(F("etiket çakışması")); break;
    case MFRC522::STATUS_TIMEOUT: console.println(F("haberleşme zaman aşımı")); break;
    case MFRC522::STATUS_NO_ROOM: console.println(F("tampon yetersiz")); break;
    case MFRC522::STATUS_INTERNAL_ERROR: console.println(F("iç hata")); break;
    case MFRC522::STATUS_INVALID: console.println(F("geçersiz parametre")); break;
    case MFRC522::STATUS_CRC_WRONG: console.println(F("CRC uyuşmazlığı")); break;
    case MFRC522::STATUS_MIFARE_NACK: console.println(F("etiket işlemi reddetti (NAK)")); break;
    default: console.println(F("bilinmeyen MFRC522 hatası")); break;
  }
}

void finishCard() {
  // Seçilmiş kartı durdur; şifrelemeyi sonraki kart için kapat.
  const MFRC522::StatusCode status = reader.PICC_HaltA();
  if (status != MFRC522::STATUS_OK) {
    printStatus("Kart durdurulamadı", status);
  }
  reader.PCD_StopCrypto1();
}

bool validUtf8(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length;) {
    const uint8_t c = data[i];
    if (c == 0) return false;
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t following = 0;
    uint32_t codePoint = 0;
    uint32_t minimum = 0;
    if ((c & 0xE0) == 0xC0) {
      following = 1;
      codePoint = c & 0x1F;
      minimum = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      following = 2;
      codePoint = c & 0x0F;
      minimum = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      following = 3;
      codePoint = c & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (i + following >= length) return false;
    for (size_t j = 1; j <= following; ++j) {
      const uint8_t continuation = data[i + j];
      if ((continuation & 0xC0) != 0x80) return false;
      codePoint = (codePoint << 6) | (continuation & 0x3F);
    }
    if (codePoint < minimum || codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF)) return false;
    i += following + 1;
  }
  return true;
}

void printHelp() {
  console.println(F("Komutlar:"));
  console.println(F("  info          - Etiketin UID ve türünü göster"));
  console.println(F("  write ADET METIN - 1-65535 kez, en çok 43 UTF-8 byte yaz"));
  console.println(F("  read          - Kayıtlı metni oku"));
  console.println(F("  cancel        - Bekleyen işlemi iptal et"));
  console.println(F("  help          - Bu listeyi göster"));
}

void startOperation(PendingOperation operation) {
  pending = operation;
  pendingSince = millis();
  lastCardPoll = pendingSince - CARD_POLL_MS;
  lastScanErrorAt = pendingSince - 1000;
  waitForCardRemoval = false;
  if (operation == PendingOperation::Write) {
    if (writeTarget == 1) console.println(F("5 saniye boyunca yazılabilir etiket taranıyor..."));
    else console.println(F("Hedef sayıya ulaşılana kadar yazılabilir etiket taranıyor..."));
  } else {
    console.println(F("Etiket taranıyor (30 saniye)."));
  }
}

void handleCommand(const char *command, size_t length) {
  if (strcmp(command, "help") == 0) {
    printHelp();
    return;
  }
  if (strcmp(command, "cancel") == 0) {
    pending = PendingOperation::None;
    waitForCardRemoval = false;
    console.println(F("Bekleyen işlem iptal edildi."));
    return;
  }
  // Yeni bir komut eski bekleyen yazmayı yanlışlıkla sürdürmemeli.
  pending = PendingOperation::None;
  waitForCardRemoval = false;
  if (!readerReady) {
    console.println(F("Hata: RC522 bağlantısı hazır değil. Bağlantıları kontrol edin; modül her saniye yeniden yoklanıyor."));
    return;
  }
  if (strcmp(command, "info") == 0) {
    startOperation(PendingOperation::Info);
  } else if (strcmp(command, "read") == 0) {
    startOperation(PendingOperation::Read);
  } else if (strncmp(command, "write ", 6) == 0) {
    size_t textStart = 6;
    uint16_t count = 0;
    while (textStart < length && command[textStart] >= '0' && command[textStart] <= '9') {
      const uint8_t digit = command[textStart] - '0';
      if (count > (UINT16_MAX - digit) / 10) {
        console.println(F("Hata: Adet 1-65535 arasında olmalı."));
        return;
      }
      count = count * 10 + digit;
      ++textStart;
    }
    if (count == 0 || textStart >= length || command[textStart] != ' ') {
      console.println(F("Hata: Kullanım write ADET METIN (adet 1-65535)."));
      return;
    }
    ++textStart;
    const size_t size = length - textStart;
    if (size == 0) {
      console.println(F("Hata: Yazılacak metin boş olamaz."));
      return;
    }
    if (size > MAX_TEXT_BYTES) {
      console.println(F("Hata: Metin 43 UTF-8 byte sınırını aşıyor; hiçbir byte kesilmedi."));
      return;
    }
    if (!validUtf8(reinterpret_cast<const uint8_t *>(command + textStart), size)) {
      console.println(F("Hata: Metin geçerli UTF-8 değil veya NUL içeriyor."));
      return;
    }
    memcpy(textToWrite, command + textStart, size);
    textLength = static_cast<uint8_t>(size);
    writeTarget = count;
    writeCompleted = 0;
    console.println(F("Uyarı: Etiketteki sektör 1 veri blokları 4, 5 ve 6 üzerine yazılacak."));
    startOperation(PendingOperation::Write);
  } else {
    console.println(F("Bilinmeyen komut. Liste için help yazın."));
  }
}

void pollSerial() {
  // Bir döngüde sınırlı sayıda byte işle; kart ve zaman aşımı kontrolü aksamasın.
  for (size_t count = 0; count < 64 && console.available() > 0; ++count) {
    const char c = static_cast<char>(console.read());
    if (c == '\r' || c == '\n') {
      if (commandOverflow) {
        pending = PendingOperation::None;
        console.println(F("Hata: Seri komut tamponu taştı; komut reddedildi."));
      } else if (commandLength > 0) {
        commandBuffer[commandLength] = '\0';
        handleCommand(commandBuffer, commandLength);
      }
      commandLength = 0;
      commandOverflow = false;
    } else if (!commandOverflow) {
      if (commandLength < COMMAND_BUFFER_SIZE - 1) {
        commandBuffer[commandLength++] = c;
      } else {
        commandOverflow = true;
      }
    }
  }
}

bool classicType(MFRC522::PICC_Type type) {
  return type == MFRC522::PICC_TYPE_MIFARE_MINI ||
         type == MFRC522::PICC_TYPE_MIFARE_1K ||
         type == MFRC522::PICC_TYPE_MIFARE_4K;
}

void printCardInfo(MFRC522::PICC_Type type) {
  console.print(F("UID: "));
  for (byte i = 0; i < reader.uid.size; ++i) {
    if (i) console.print(':');
    if (reader.uid.uidByte[i] < 0x10) console.print('0');
    console.print(reader.uid.uidByte[i], HEX);
  }
  console.println();
  console.print(F("Çip türü: "));
  console.println(reader.PICC_GetTypeName(type));
}

bool rememberCard(unsigned long now) {
  const bool alreadyShown = reader.uid.size == lastUidSize &&
      memcmp(reader.uid.uidByte, lastUid, lastUidSize) == 0 &&
      now - lastSeenAt < SAME_CARD_GAP_MS;
  lastUidSize = reader.uid.size;
  memcpy(lastUid, reader.uid.uidByte, lastUidSize);
  lastSeenAt = now;
  return alreadyShown;
}

bool authenticateSector() {
  const MFRC522::StatusCode status = reader.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, DATA_BLOCKS[0], &defaultKey, &reader.uid);
  if (status != MFRC522::STATUS_OK) {
    printStatus("Key A (FF FF FF FF FF FF) ile kimlik doğrulama başarısız", status);
    return false;
  }
  return true;
}

bool readBlocks(uint8_t (&record)[RECORD_SIZE]) {
  for (size_t i = 0; i < 3; ++i) {
    uint8_t buffer[18];  // 16 veri byte'ı + 2 CRC byte'ı
    byte bufferLength = sizeof(buffer);  // MIFARE_Read bu değeri değiştirir.
    const MFRC522::StatusCode status = reader.MIFARE_Read(
        DATA_BLOCKS[i], buffer, &bufferLength);
    if (status != MFRC522::STATUS_OK) {
      console.print(F("Blok "));
      console.print(DATA_BLOCKS[i]);
      printStatus(" okuma hatası", status);
      return false;
    }
    if (bufferLength < 16) {
      console.print(F("Hata: Blok "));
      console.print(DATA_BLOCKS[i]);
      console.println(F(" eksik okundu."));
      return false;
    }
    memcpy(record + i * 16, buffer, 16);
  }
  return true;
}

bool writeBlocks(uint8_t (&record)[RECORD_SIZE]) {
  for (size_t i = 0; i < 3; ++i) {
    const MFRC522::StatusCode status = reader.MIFARE_Write(
        DATA_BLOCKS[i], record + i * 16, 16);
    if (status != MFRC522::STATUS_OK) {
      console.print(F("Blok "));
      console.print(DATA_BLOCKS[i]);
      printStatus(" yazma hatası", status);
      return false;
    }
    console.print(F("Blok "));
    console.print(DATA_BLOCKS[i]);
    console.println(F(" yazıldı."));
  }
  return true;
}

void readText() {
  uint8_t record[RECORD_SIZE];
  if (!readBlocks(record)) {
    console.println(F("Okuma tamamlanamadı."));
    return;
  }
  if (memcmp(record, "TAG1", 4) != 0 || record[4] > MAX_TEXT_BYTES ||
      !validUtf8(record + 5, record[4])) {
    console.println(F("Hata: İmza, uzunluk veya UTF-8 geçersiz; veri metin olarak gösterilmedi."));
    return;
  }
  for (size_t i = 5 + record[4]; i < RECORD_SIZE; ++i) {
    if (record[i] != 0) {
      console.println(F("Hata: Sıfır dolgusu geçersiz; veri metin olarak gösterilmedi."));
      return;
    }
  }
  console.print(F("Kayıtlı metin: "));
  console.write(record + 5, record[4]);
  console.println();
}

bool writeText() {
  uint8_t record[RECORD_SIZE] = {};
  memcpy(record, "TAG1", 4);
  record[4] = textLength;
  memcpy(record + 5, textToWrite, textLength);

  uint8_t existing[RECORD_SIZE];
  if (!readBlocks(existing)) {
    console.println(F("Yazma öncesi okuma başarısız; güvenlik için bu etiket atlandı."));
    return false;
  }
  if (memcmp(existing, "TAG1", 4) == 0 && existing[4] <= MAX_TEXT_BYTES) {
    const uint8_t existingLength = existing[4];
    if ((existingLength == textLength &&
         memcmp(existing + 5, textToWrite, textLength) == 0) ||
        (hasLastWrittenText && existingLength == lastWrittenLength &&
         memcmp(existing + 5, lastWrittenText, lastWrittenLength) == 0)) {
      console.println(F("Etiket mevcut veya önceki mesajı taşıyor; yazma atlandı, sayaç değişmedi."));
      return false;
    }
  }

  console.println(F("Yazma başladı."));
  if (!writeBlocks(record)) {
    console.println(F("Yazma tamamlanamadı; bazı bloklar değişmiş olabilir."));
    return false;
  }
  console.println(F("48 byte geri okunup doğrulanıyor..."));
  uint8_t verified[RECORD_SIZE];
  if (!readBlocks(verified)) {
    console.println(F("Yazma sonrası doğrulama okunamadı; başarı bildirilmiyor."));
    return false;
  }
  if (memcmp(record, verified, RECORD_SIZE) != 0) {
    console.println(F("Hata: Yazılan 48 byte ile geri okunan veri eşleşmiyor."));
    return false;
  }
  console.println(F("Yazma doğrulandı."));
  memcpy(lastWrittenText, textToWrite, textLength);
  lastWrittenLength = textLength;
  hasLastWrittenText = true;
  return true;
}

void pollNetwork() {
  if (RFID_WIFI_SSID[0] == '\0' || RFID_OTA_PASSWORD[0] == '\0') return;
  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (networkStarted) {
      console.dropClient();
      networkStarted = false;
    }
    if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
      lastWifiAttempt = now;
      WiFi.begin(RFID_WIFI_SSID, RFID_WIFI_PASSWORD);
    }
    return;
  }
  if (!networkStarted) {
    networkConsole.begin();
    ArduinoOTA.setHostname("rfid-rc522");
    ArduinoOTA.setPassword(RFID_OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      pending = PendingOperation::None;
      waitForCardRemoval = false;
      console.println(F("OTA başladı; bekleyen RFID işlemi iptal edildi."));
    });
    ArduinoOTA.begin();
    networkStarted = true;
    console.print(F("Wi-Fi hazır. IP: "));
    console.println(WiFi.localIP());
    console.println(F("Ağ konsolu: TCP 2323; OTA: TCP 3232."));
  }
  ArduinoOTA.handle();
  WiFiClient incoming = networkConsole.accept();
  if (incoming) {
    if (console.connected()) console.dropClient();
    console.setClient(incoming);
    console.println(F("RFID ağ konsolu bağlandı. Komutlar için help yazın."));
  }
}

void pollCard() {
  const unsigned long now = millis();
  const bool singleWrite = pending == PendingOperation::Write && writeTarget == 1;
  const unsigned long timeout = singleWrite ? WRITE_WAIT_TIMEOUT_MS : WAIT_TIMEOUT_MS;
  if (pending != PendingOperation::None &&
      (pending != PendingOperation::Write || singleWrite) && now - pendingSince >= timeout) {
    const bool wasWrite = pending == PendingOperation::Write;
    pending = PendingOperation::None;
    if (wasWrite) console.println(F("Zaman aşımı: 5 saniyede yazılabilir etiket bulunamadı; yazma yapılmadı."));
    else console.println(F("Zaman aşımı: 30 saniyede etiket algılanmadı."));
  }
  if (now - lastCardPoll < CARD_POLL_MS) return;
  lastCardPoll = now;

  // WUPA, anten üzerinde önceden bekleyen HALT durumundaki etiketi de görür.
  byte atqa[2];
  byte atqaLength = sizeof(atqa);
  const MFRC522::StatusCode wakeStatus = reader.PICC_WakeupA(atqa, &atqaLength);
  if (wakeStatus == MFRC522::STATUS_TIMEOUT) {
    waitForCardRemoval = false;
    return;  // Etiket yok.
  }
  if (wakeStatus != MFRC522::STATUS_OK) {
    if (pending != PendingOperation::None && now - lastScanErrorAt >= 1000) {
      printStatus("Etiket tarama hatası", wakeStatus);
      lastScanErrorAt = now;
    }
    return;
  }
  const MFRC522::StatusCode selectStatus = reader.PICC_Select(&reader.uid);
  if (selectStatus != MFRC522::STATUS_OK) {
    if (pending != PendingOperation::None) printStatus("Etiket UID seçimi başarısız", selectStatus);
    reader.PCD_StopCrypto1();
    return;
  }

  if (pending == PendingOperation::Write && waitForCardRemoval) {
    finishCard();
    return;
  }

  const PendingOperation operation = pending;
  const bool alreadyShown = rememberCard(now);
  const MFRC522::PICC_Type type = reader.PICC_GetType(reader.uid.sak);
  if (operation == PendingOperation::None) {
    if (!alreadyShown) {
      console.println(F("Etiket algılandı; yazma için write ADET METIN komutu verin."));
      printCardInfo(type);
    }
    finishCard();
    return;
  }

  if (operation != PendingOperation::Write || writeTarget == 1) {
    pending = PendingOperation::None;
  } else {
    // Aynı kart anten üzerinde kaldığı sürece ikinci kez yazma.
    waitForCardRemoval = true;
  }
  printCardInfo(type);
  if (operation == PendingOperation::Info) {
    if (!classicType(type)) console.println(F("Bu etiket türü veri okuma/yazma için desteklenmiyor."));
  } else if (!classicType(type)) {
    console.println(F("Bu etiket türü desteklenmiyor; okuma/yazma reddedildi."));
  } else if (authenticateSector()) {
    if (operation == PendingOperation::Read) readText();
    else if (writeText()) {
      ++writeCompleted;
      console.print(F("İlerleme: "));
      console.print(writeCompleted);
      console.print('/');
      console.println(writeTarget);
      if (writeCompleted == writeTarget) {
        pending = PendingOperation::None;
        waitForCardRemoval = false;
        console.println(F("Hedef tamamlandı; yazma modu kapatıldı."));
      }
    }
  }
  if (operation == PendingOperation::Write && pending == PendingOperation::Write &&
      waitForCardRemoval) {
    console.println(F("Yazma modu açık; sıradaki deneme için etiketi uzaklaştırın."));
  }
  finishCard();
}

void pollReader() {
  if (readerReady) return;
  const unsigned long now = millis();
  if (now - lastReaderProbe < READER_PROBE_MS) return;
  lastReaderProbe = now;
  const byte version = reader.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) return;
  reader.PCD_Init();
  const byte initializedVersion = reader.PCD_ReadRegister(MFRC522::VersionReg);
  readerReady = initializedVersion != 0x00 && initializedVersion != 0xFF;
  if (readerReady) {
    console.print(F("RC522 yeniden bağlandı; VersionReg: 0x"));
    console.println(initializedVersion, HEX);
    console.println(F("Sürekli etiket taraması aktif."));
  }
}
}  // namespace

void setup() {
  console.begin(115200);
  if (RFID_WIFI_SSID[0] != '\0' && RFID_OTA_PASSWORD[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(RFID_WIFI_SSID, RFID_WIFI_PASSWORD);
    lastWifiAttempt = millis();
  } else {
    console.println(F("Wi-Fi/OTA kapalı: wifi_config.h içinde SSID ve OTA parolası ayarlayın."));
  }
  SPI.begin(18, 19, 23, PIN_SS);
  reader.PCD_Init();
  const byte version = reader.PCD_ReadRegister(MFRC522::VersionReg);
  readerReady = version != 0x00 && version != 0xFF;
  console.print(F("RC522 VersionReg: 0x"));
  if (version < 0x10) console.print('0');
  console.println(version, HEX);
  if (readerReady) console.println(F("RC522 bağlantısı hazır. Sürekli etiket taraması aktif."));
  else console.println(F("Hata: RC522 yanıt vermiyor. 3V3, GND ve SPI bağlantılarını kontrol edin."));
  printHelp();
}

void loop() {
  pollNetwork();
  pollSerial();
  pollReader();
  if (readerReady) pollCard();
}
