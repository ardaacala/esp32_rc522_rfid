#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <cstring>

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
constexpr size_t COMMAND_BUFFER_SIZE = 512;
constexpr size_t MAX_WRITE_STEPS = 8;
constexpr unsigned long WAIT_TIMEOUT_MS = 30000;
constexpr unsigned long WRITE_WAIT_TIMEOUT_MS = 5000;
constexpr unsigned long CARD_POLL_MS = 50;
constexpr unsigned long READER_PROBE_MS = 1000;
constexpr unsigned long SAME_CARD_GAP_MS = 700;
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr char AP_SSID[] = "RFID_RC522";
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
struct WriteStep {
  uint16_t target;
  uint8_t length;
  uint8_t text[MAX_TEXT_BYTES];
};
WriteStep writeSteps[MAX_WRITE_STEPS];
size_t writeStepCount = 0;
size_t writeStepIndex = 0;
uint8_t lastWrittenText[MAX_TEXT_BYTES];
uint8_t lastWrittenLength = 0;
bool hasLastWrittenText = false;
unsigned long lastWifiAttempt = 0;
bool apReady = false;
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
  // A moving tag can answer HALT or leave the field mid-command. This is best effort.
  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();
}

void initializeReader() {
  // Force a hardware reset; a soft reset alone cannot recover a silent RC522.
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(2);
  digitalWrite(PIN_RST, HIGH);
  delay(50);
  reader.PCD_Init();
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
  console.println(F("  write ADET METIN [ADET METIN...] - sırayla yaz (metin en çok 43 byte)"));
  console.println(F("  Örnek: write 2 ILK 3 IKINCI; sayı içeren metni tırnaklayın"));
  console.println(F("  read          - Kayıtlı metni oku"));
  console.println(F("  cancel        - Bekleyen işlemi iptal et"));
  console.println(F("  help          - Bu listeyi göster"));
}

bool parseWriteSteps(const char *command, size_t length) {
  writeStepCount = 0;
  size_t position = 6;
  while (position < length) {
    if (writeStepCount == MAX_WRITE_STEPS) {
      console.println(F("Hata: En çok 8 yazma aşaması verilebilir."));
      return false;
    }
    uint16_t count = 0;
    const size_t countStart = position;
    while (position < length && command[position] >= '0' && command[position] <= '9') {
      const uint8_t digit = command[position] - '0';
      if (count > (UINT16_MAX - digit) / 10) {
        console.println(F("Hata: Adet 1-65535 arasında olmalı."));
        return false;
      }
      count = count * 10 + digit;
      ++position;
    }
    if (position == countStart || count == 0 || position >= length || command[position] != ' ') {
      console.println(F("Hata: Kullanım write ADET METIN [ADET METIN...] (adet 1-65535)."));
      return false;
    }
    ++position;
    const bool quoted = position < length && command[position] == '"';
    if (quoted) ++position;
    const size_t textStart = position;
    size_t textEnd = length;
    size_t nextCount = length;
    if (quoted) {
      while (position < length && command[position] != '"') ++position;
      if (position == length) {
        console.println(F("Hata: Metnin kapanış tırnağı eksik."));
        return false;
      }
      textEnd = position++;
      if (position < length) {
        if (command[position] != ' ') {
          console.println(F("Hata: Tırnaklı metinden sonra boşluk ve adet gelmeli."));
          return false;
        }
        nextCount = position + 1;
        if (nextCount == length) {
          console.println(F("Hata: Sonraki aşamanın adedi eksik."));
          return false;
        }
      }
    } else {
      for (size_t i = position; i + 2 < length; ++i) {
        if (command[i] != ' ' || command[i + 1] < '0' || command[i + 1] > '9') continue;
        size_t digitsEnd = i + 1;
        while (digitsEnd < length && command[digitsEnd] >= '0' && command[digitsEnd] <= '9') ++digitsEnd;
        if (digitsEnd < length && command[digitsEnd] == ' ') {
          textEnd = i;
          nextCount = i + 1;
          break;
        }
      }
    }
    const size_t size = textEnd - textStart;
    if (size == 0) {
      console.println(F("Hata: Yazılacak metin boş olamaz."));
      return false;
    }
    if (size > MAX_TEXT_BYTES) {
      console.println(F("Hata: Metin 43 UTF-8 byte sınırını aşıyor; hiçbir byte kesilmedi."));
      return false;
    }
    if (!validUtf8(reinterpret_cast<const uint8_t *>(command + textStart), size)) {
      console.println(F("Hata: Metin geçerli UTF-8 değil veya NUL içeriyor."));
      return false;
    }
    WriteStep &step = writeSteps[writeStepCount++];
    step.target = count;
    step.length = static_cast<uint8_t>(size);
    memcpy(step.text, command + textStart, size);
    position = nextCount;
  }
  return writeStepCount > 0;
}

void activateWriteStep(size_t index) {
  writeStepIndex = index;
  const WriteStep &step = writeSteps[index];
  memcpy(textToWrite, step.text, step.length);
  textLength = step.length;
  writeTarget = step.target;
  writeCompleted = 0;
  console.print(F("Yazma aşaması "));
  console.print(index + 1);
  console.print('/');
  console.print(writeStepCount);
  console.print(F("; hedef: "));
  console.println(writeTarget);
}

void startOperation(PendingOperation operation) {
  pending = operation;
  pendingSince = millis();
  lastCardPoll = pendingSince - CARD_POLL_MS;
  lastScanErrorAt = pendingSince - 1000;
  waitForCardRemoval = false;
  if (operation == PendingOperation::Write) {
    if (writeStepCount == 1 && writeTarget == 1) console.println(F("5 saniye boyunca yazılabilir etiket taranıyor..."));
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
    if (!parseWriteSteps(command, length)) return;
    activateWriteStep(0);
    console.println(F("Uyarı: Classic blok 4-6 veya Ultralight sayfa 4-15 üzerine yazılacak."));
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

bool supportedType(MFRC522::PICC_Type type) {
  return classicType(type) || type == MFRC522::PICC_TYPE_MIFARE_UL;
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
      memcmp(reader.uid.uidByte, lastUid, lastUidSize) == 0;
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

bool readRecord(MFRC522::PICC_Type type, uint8_t (&record)[RECORD_SIZE]) {
  for (size_t i = 0; i < 3; ++i) {
    uint8_t buffer[18];  // 16 veri byte'ı + 2 CRC byte'ı
    byte bufferLength = sizeof(buffer);  // MIFARE_Read bu değeri değiştirir.
    const byte address = type == MFRC522::PICC_TYPE_MIFARE_UL
        ? static_cast<byte>(4 + i * 4) : DATA_BLOCKS[i];
    const MFRC522::StatusCode status = reader.MIFARE_Read(
        address, buffer, &bufferLength);
    if (status != MFRC522::STATUS_OK) {
      console.print(type == MFRC522::PICC_TYPE_MIFARE_UL ? F("Sayfa ") : F("Blok "));
      console.print(address);
      printStatus(" okuma hatası", status);
      return false;
    }
    if (bufferLength < 16) {
      console.print(type == MFRC522::PICC_TYPE_MIFARE_UL ? F("Hata: Sayfa ") : F("Hata: Blok "));
      console.print(address);
      console.println(F(" eksik okundu."));
      return false;
    }
    memcpy(record + i * 16, buffer, 16);
  }
  return true;
}

bool writeRecord(MFRC522::PICC_Type type, uint8_t (&record)[RECORD_SIZE]) {
  if (type == MFRC522::PICC_TYPE_MIFARE_UL) {
    for (size_t i = 0; i < 12; ++i) {
      const byte page = static_cast<byte>(4 + i);
      const MFRC522::StatusCode status = reader.MIFARE_Ultralight_Write(
          page, record + i * 4, 4);
      if (status != MFRC522::STATUS_OK) {
        console.print(F("Sayfa "));
        console.print(page);
        printStatus(" yazma hatası", status);
        return false;
      }
    }
    return true;
  }
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

void readText(MFRC522::PICC_Type type) {
  uint8_t record[RECORD_SIZE];
  if (!readRecord(type, record)) {
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

bool writeText(MFRC522::PICC_Type type) {
  uint8_t record[RECORD_SIZE] = {};
  memcpy(record, "TAG1", 4);
  record[4] = textLength;
  memcpy(record + 5, textToWrite, textLength);

  uint8_t existing[RECORD_SIZE];
  if (!readRecord(type, existing)) {
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
  if (!writeRecord(type, record)) {
    console.println(F("Yazma tamamlanamadı; bazı bloklar veya sayfalar değişmiş olabilir."));
    return false;
  }
  console.println(F("48 byte geri okunup doğrulanıyor..."));
  uint8_t verified[RECORD_SIZE];
  if (!readRecord(type, verified)) {
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
  const unsigned long now = millis();
  if (!apReady) {
    if (now - lastWifiAttempt < WIFI_RETRY_MS) return;
    lastWifiAttempt = now;
    apReady = WiFi.softAP(AP_SSID);
    if (!apReady) return;
  }
  if (!networkStarted) {
    networkConsole.begin();
    ArduinoOTA.setHostname("rfid-rc522");
    ArduinoOTA.onStart([]() {
      pending = PendingOperation::None;
      waitForCardRemoval = false;
      console.println(F("OTA başladı; bekleyen RFID işlemi iptal edildi."));
    });
    ArduinoOTA.begin();
    networkStarted = true;
    console.print(F("Şifresiz Wi-Fi ağı: "));
    console.println(AP_SSID);
    console.print(F("ESP IP: "));
    console.println(WiFi.softAPIP());
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
  const bool singleWrite = pending == PendingOperation::Write && writeStepCount == 1 && writeTarget == 1;
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
    if (now - lastSeenAt >= SAME_CARD_GAP_MS) lastUidSize = 0;
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
    // A mechanical feeder may replace a tag without an empty antenna interval.
    if (reader.uid.size == lastUidSize &&
        memcmp(reader.uid.uidByte, lastUid, lastUidSize) == 0) {
      finishCard();
      return;
    }
    waitForCardRemoval = false;
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

  if (operation != PendingOperation::Write || singleWrite) {
    pending = PendingOperation::None;
  } else {
    // Aynı kart anten üzerinde kaldığı sürece ikinci kez yazma.
    waitForCardRemoval = true;
  }
  printCardInfo(type);
  if (operation == PendingOperation::Info) {
    if (!supportedType(type)) console.println(F("Bu etiket türü veri okuma/yazma için desteklenmiyor."));
  } else if (!supportedType(type)) {
    console.println(F("Bu etiket türü desteklenmiyor; okuma/yazma reddedildi."));
  } else if (!classicType(type) || authenticateSector()) {
    if (operation == PendingOperation::Read) readText(type);
    else if (writeText(type)) {
      ++writeCompleted;
      console.print(F("İlerleme: "));
      console.print(writeCompleted);
      console.print('/');
      console.println(writeTarget);
      if (writeCompleted == writeTarget) {
        if (writeStepIndex + 1 < writeStepCount) {
          console.println(F("Aşama tamamlandı; sıradaki metne geçiliyor."));
          activateWriteStep(writeStepIndex + 1);
        } else {
          pending = PendingOperation::None;
          waitForCardRemoval = false;
          console.println(F("Hedef tamamlandı; yazma modu kapatıldı."));
        }
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
  const unsigned long now = millis();
  if (now - lastReaderProbe < READER_PROBE_MS) return;
  lastReaderProbe = now;
  if (readerReady) {
    const byte version = reader.PCD_ReadRegister(MFRC522::VersionReg);
    if (version != 0x00 && version != 0xFF) return;
    readerReady = false;
    pending = PendingOperation::None;
    waitForCardRemoval = false;
    console.println(F("RC522 bağlantısı kesildi; okuyucu yeniden başlatılıyor."));
  }
  // A late power-up may return 0xFF until the RC522 is initialized again.
  initializeReader();
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
  WiFi.mode(WIFI_AP);
  apReady = WiFi.softAP(AP_SSID);
  lastWifiAttempt = millis();
  if (!apReady) console.println(F("Wi-Fi ağı başlatılamadı; yeniden denenecek."));
  SPI.begin(18, 19, 23, PIN_SS);
  initializeReader();
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
