# ESP32 ve RC522 ile USB seri RFID okuma/yazma

Bu bağımsız PlatformIO projesi ESP32-WROOM-32 (`esp32dev`) ve HW-868 Mini RFID RC522 ile çalışır. **MIFARE Classic Mini, 1K ve 4K** etiketlerin sektör 1 veri bloklarını (4, 5, 6) ve **MIFARE Ultralight / Ultralight C** etiketlerin kullanıcı sayfalarını (4-15) okur ve yazar. ESP kendi şifresiz Wi-Fi ağını yayınlar; telefon veya harici Wi-Fi ağı gerekmez.

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
| `write ADET METIN [ADET METIN...]` | En çok 8 metni sırayla yazar. Her aşama 1-65535 doğrulanmış yazma ister. Tek aşamada `ADET` 1 ise 5 saniye etiket bekler; diğer durumlarda bütün hedefler tamamlanana veya `cancel` verilene kadar yazma modunda kalır. |
| `read` | 30 saniye etiket bekler ve kayıtlı metni gösterir. |
| `read on` | Sürekli okuma açar. Her yeni etiketi okur; aynı etiket anten üzerinde kaldığı sürece çıktıyı tekrarlamaz. Zaman aşımı yoktur. |
| `read off` | Sürekli okumayı kapatır. `help`, `info`, `read`, `write` veya başka bir komut da sürekli okumayı kapatır. |
| `cancel` | Bekleyen etiket işlemini iptal eder. |

Örnek:

```text
write 2 SENARYO_01
Uyarı: Classic blok 4-6 veya Ultralight sayfa 4-15 üzerine yazılacak.
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

Sıralı yazma örneği: `write 2 ILK 3 IKINCI`. İlk metin iki farklı etikete doğrulanarak yazıldıktan sonra ikinci metnin üç etiketlik sayacı başlar. Boşluk ve sayı içeren bir metnin aşama ayırıcı sanılmaması için tırnak kullanın: `write 2 "Seri 1 A" 3 "Seri 2 B"`. Her metin en çok 43 UTF-8 byte olabilir. Aşamalar arasında aynı etiketin yeniden yazılmaması için etiketin UID'si ve mevcut kaydı kontrol edilir; yeni UID geldiğinde mekanik besleyici boş anten aralığı bırakmasa da sıradaki etiket işlenebilir.

Kesintisiz okuma için `read on` yazın. Her yeni UID'nin kayıtlı metni görüntülenir. Aynı etiketi yeniden okutmak için anten alanından kısa süre uzaklaştırıp tekrar yaklaştırın. `read off` veya başka bir komutla bu modu kapatın. Tek seferlik `read` komutu 30 saniyelik bekleme davranışını korur.

RC522 sürekli tarama yapar ve algıladığı etiketi seri monitörde gösterir. Etiketin yaklaşması tek başına yazma başlatmaz. `write 1 METIN` verildiğinde etiket zaten anten üzerindeyse de algılanır. Tekli yazmada başarılı veya hatalı kart denemesinden sonra komut temizlenir. Çoklu yazmada yalnızca geri okunup doğrulanan yazmalar sayılır; başarısız deneme veya bekleme yazma modunu kapatmaz. Her denemeden sonra mevcut etiketi uzaklaştırın ve sıradaki etiketi yaklaştırın. Etiketi anten üzerinde tutmak tekrar tekrar yazmaya yol açmaz. `cancel` ile işlemi iptal edebilirsiniz. Yeni bir komut mevcut yazma işlemini sonlandırır.

SPI hızı, kablo kaynaklı iletişim sorunlarını azaltmak için 1 MHz olarak ayarlanmıştır. `VersionReg` sürekli `0x00` veya `0xFF` kalıyorsa fiziksel bağlantıları ve 3V3 beslemesini kontrol edin; bu durumda yazma başlamaz.

## Veri düzeni ve sınırlar

Classic blok 4-6 veya Ultralight sayfa 4-15 toplam 48 byte tutar: `TAG1` imzası (4 byte), UTF-8 byte uzunluğu (1 byte), en çok 43 byte metin ve sıfır dolgusu. Türkçe karakterler birden fazla byte kullanabilir. Sınırı aşan veya geçersiz UTF-8 içeren komut kesilmeden reddedilir. Okurken imza, uzunluk ve UTF-8 doğrulanır. Ultralight'ta sayfa 0-3 (UID, kilit bitleri ve OTP) değiştirilmez. Bu 48 byte'lık alanın önceki NDEF veya başka uygulama verisi yazma sırasında değişir.

Yazma sonrası 48 byte geri okunup karşılaştırılır. Eşleşme olmadan başarı mesajı verilmez. Classic için yalnızca varsayılan Key A (`FF FF FF FF FF FF`) denenir; kimlik doğrulama başarısızsa işlem hata ile biter. Classic blok 0, sektör trailer bloğu 7, UID, anahtarlar ve erişim bitleri değiştirilmez. Ultralight için Classic kimlik doğrulaması uygulanmaz; kilitli veya korumalı sayfalarda yazma reddedilebilir. Desteklenmeyen çip türlerinde bilgi gösterilir ve yazma reddedilir. Kısmi yazma hatasında bazı veri blokları veya sayfalar değişmiş olabilir; yeniden `read` ile denetleyin.

## Çoklu yazmada ön okuma

Her yazma denemesinden hemen önce Classic blok 4-6 veya Ultralight sayfa 4-15 okunur. Okuma başarısızsa güvenlik için yazma yapılmaz. Etiketteki `TAG1` metni, yazılacak metinle veya bu çalışmada en son başarıyla yazılmış metinle aynıysa etiket atlanır. Atlanan etiket hedef sayıya eklenmez. Böylece mekanik besleyici daha önce işlenmiş bir etiketi yeniden antene getirse bile içerik değiştirilmez. Farklı içerikli etiketler normal yazma akışına devam eder.

## ESP'nin kendi Wi-Fi ağı, ağ konsolu ve OTA

ESP açıldığında **RFID_RC522** adlı şifresiz Wi-Fi ağını yayınlar. Telefonu veya bilgisayarı bu ağa bağlayın. Ağın internet sağlamaması normaldir. ESP'nin IP adresi USB seri monitöründe (115200 baud) yazılır; varsayılan SoftAP adresi **192.168.4.1**'dir.

Bir TCP terminaliyle `telnet 192.168.4.1 2323` veya `nc 192.168.4.1 2323` komutunu kullanarak ağ konsoluna bağlanın. USB'deki `help`, `read`, `write ADET METIN` ve `cancel` komutları burada da çalışır; çıktı iki konsola birden gönderilir. Aynı anda bir ağ istemcisi kabul edilir.

İlk firmware yüklemesini USB üzerinden yapın. Daha sonraki yüklemelerde bilgisayar **RFID_RC522** ağına bağlıyken OTA kullanın:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev_ota -t upload --upload-port 192.168.4.1
```

OTA, [PlatformIO'nun ESP32 `espota` yükleyicisini](https://docs.platformio.org/en/latest/platforms/espressif32.html) ve varsayılan **3232** portunu kullanır. OTA başlarken bekleyen RFID işlemi iptal edilir. Wi-Fi ağı, ağ konsolu ve OTA şifresizdir; ağın kapsama alanındaki kişiler erişebilir.
