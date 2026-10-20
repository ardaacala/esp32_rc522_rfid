# ESP32 ve RC522 ile USB seri RFID okuma/yazma

Bu bağımsız PlatformIO projesi ESP32-WROOM-32 (`esp32dev`) ve HW-868 Mini RFID RC522 ile çalışır. Telefon veya Wi-Fi gerekmez. Yalnızca **MIFARE Classic Mini, 1K ve 4K** etiketlerin sektör 1 veri blokları (4, 5, 6) için okuma ve yazma yapar.

## Bağlantılar

| RC522 | ESP32 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SCK | GPIO18 |
| MISO | GPIO19 |
| MOSI | GPIO23 |
| SDA/SS | GPIO21 |
| RST | GPIO22 |
| IRQ | Bağlanmaz |

RC522 üzerindeki **SDA** pini bu SPI bağlantısında **SS** görevi görür. Modülü 5 V ile beslemeyin.

## Kurulum ve yükleme

1. VS Code'a PlatformIO eklentisini kurun ve bu klasörü proje olarak açın.
2. RC522 bağlantılarını yapın ve ESP32'yi USB ile bilgisayara bağlayın.
3. PlatformIO'da **Build** ve ardından **Upload** çalıştırın. Komut satırında karşılıkları `pio run` ve `pio run -t upload` komutlarıdır. Gerekirse yükleme portunu kendi kartınızın portuna göre PlatformIO'dan seçin.
4. **Serial Monitor**'ü **115200 baud** ve Newline veya Both NL & CR satır sonu ile açın (`pio device monitor -b 115200`).

Açılışta `VersionReg` ve bağlantı durumu yazılır. `0x00` veya `0xFF` yanıtı bağlantı hatası sayılır; kabloları kontrol edin. Yazılım saniyede bir yeniden yoklar ve modül yanıt vermeye başladığında taramayı açar.

## Seri komutlar

| Komut | İşlev |
| --- | --- |
| `help` | Komut listesini gösterir. |
| `info` | Yaklaştırılan etiketin UID ve çip türünü gösterir. |
| `write ADET METIN` | Metni hazırlar ve 1-65535 kez doğrulanmış yazma yapar. `ADET` 1 ise 5 saniye etiket bekler; 1'den büyükse hedef tamamlanana veya `cancel` verilene kadar yazma modunda kalır. |
| `read` | 30 saniye etiket bekler ve kayıtlı metni gösterir. |
| `cancel` | Bekleyen etiket işlemini iptal eder. |

Örnek:

```text
write 2 SENARYO_01
Uyarı: Etiketteki sektör 1 veri blokları 4, 5 ve 6 üzerine yazılacak.
Hedef sayıya ulaşılana kadar yazılabilir etiket taranıyor...
...etiketi yaklaştırın...
Yazma doğrulandı.
İlerleme: 1/2
...ilk etiketi uzaklaştırıp ikinci etiketi yaklaştırın...
Yazma doğrulandı.
İlerleme: 2/2
Hedef tamamlandı; yazma modu kapatıldı.
read
Etiket taranıyor (30 saniye).
...etiket anten üzerinde kalabilir...
Kayıtlı metin: SENARYO_01
```

RC522 sürekli tarama yapar ve algıladığı etiketi seri monitörde gösterir. Etiketin yaklaşması tek başına yazma başlatmaz. `write 1 METIN` verildiğinde etiket zaten anten üzerindeyse de algılanır. Tekli yazmada başarılı veya hatalı kart denemesinden sonra komut temizlenir. Çoklu yazmada yalnızca geri okunup doğrulanan yazmalar sayılır; başarısız deneme veya bekleme yazma modunu kapatmaz. Her denemeden sonra mevcut etiketi uzaklaştırın ve sıradaki etiketi yaklaştırın. Etiketi anten üzerinde tutmak tekrar tekrar yazmaya yol açmaz. `cancel` ile işlemi iptal edebilirsiniz. Yeni bir komut mevcut yazma işlemini sonlandırır.

SPI hızı, kablo kaynaklı iletişim sorunlarını azaltmak için 1 MHz olarak ayarlanmıştır. `VersionReg` sürekli `0x00` veya `0xFF` kalıyorsa fiziksel bağlantıları ve 3V3 beslemesini kontrol edin; bu durumda yazma başlamaz.

## Veri düzeni ve sınırlar

Blok 4, 5 ve 6 toplam 48 byte tutar: `TAG1` imzası (4 byte), UTF-8 byte uzunluğu (1 byte), en çok 43 byte metin ve sıfır dolgusu. Türkçe karakterler birden fazla byte kullanabilir. Sınırı aşan veya geçersiz UTF-8 içeren komut kesilmeden reddedilir. Okurken imza, uzunluk ve UTF-8 doğrulanır.

Yazma sonrası üç blok geri okunur ve 48 byte'ın tümü karşılaştırılır. Eşleşme olmadan başarı mesajı verilmez. Yalnızca varsayılan Key A (`FF FF FF FF FF FF`) denenir; kimlik doğrulama başarısızsa işlem hata ile biter. Blok 0, sektör trailer bloğu 7, UID, anahtarlar ve erişim bitleri değiştirilmez. Desteklenmeyen çip türlerinde bilgi gösterilir ve yazma reddedilir. Kısmi yazma hatasında bazı veri blokları değişmiş olabilir; yeniden `read` ile denetleyin.

## Çoklu yazmada ön okuma

Her yazma denemesinden hemen önce etiketin 4, 5 ve 6 numaralı blokları okunur. Okuma başarısızsa güvenlik için yazma yapılmaz. Etiketteki `TAG1` metni, yazılacak metinle veya bu çalışmada en son başarıyla yazılmış metinle aynıysa etiket atlanır. Atlanan etiket hedef sayıya eklenmez. Böylece mekanik besleyici daha önce işlenmiş bir etiketi yeniden antene getirse bile içerik değiştirilmez. Farklı içerikli etiketler normal yazma akışına devam eder.

## Wi-Fi konsolu ve OTA

1. [`include/wifi_config.h.example`](include/wifi_config.h.example) dosyasını `include/wifi_config.h` adıyla kopyalayın. Wi-Fi adını, parolasını ve güçlü bir OTA parolasını girin. Bu yerel dosya Git tarafından yok sayılır.
2. İlk yüklemeyi USB üzerinden yapın. Açılışta USB seri monitörü (115200 baud) ESP'nin IP adresini gösterir. Wi-Fi veya OTA parolası ayarlanmamışsa ağ özellikleri kapalı kalır; USB seri monitörü çalışır.
3. Aynı yerel ağdaki bir TCP terminaliyle ESP'nin IP adresinde **2323** portuna bağlanın. Örneğin `telnet ESP_IP 2323` veya `nc ESP_IP 2323`. USB'deki `help`, `read`, `write ADET METIN` ve `cancel` komutları burada da çalışır; çıktı iki konsola birden gönderilir. Aynı anda bir ağ istemcisi kabul edilir. Ağ konsolu şifreli değildir; güvenilir yerel ağda kullanın.
4. Sonraki yüklemeler için PowerShell'de OTA parolasını oturum değişkenine atayıp ESP'nin IP adresini verin:

   ```powershell
   $env:RFID_OTA_PASSWORD = 'wifi_config.h içindeki OTA parolası'
   & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev_ota -t upload --upload-port ESP_IP
   ```

OTA, [PlatformIO'nun ESP32 `espota` yükleyicisini](https://docs.platformio.org/en/latest/platforms/espressif32.html) ve varsayılan **3232** portunu kullanır. OTA başlarken bekleyen RFID işlemi iptal edilir. Kartın IP adresi değişirse yeni IP'yi USB konsolundan veya yönlendiriciden alın. Parolayı değiştirdikten sonra ilk yeni sürümü mevcut parolayla yükleyin; sonraki yüklemelerde yeni parolayı kullanın.
