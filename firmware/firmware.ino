/*
 * WiFi консольный сервер на ESP8266 — v1.6.2
 *
 * Назначение: первичная настройка коммутаторов и МСЭ. Устройство лежит
 * в сумке и достаётся под задачу — "подключиться к незнакомой железке
 * и довести её до управляемого состояния". Отсюда решения ниже.
 *
 * - Консоль на аппаратном UART0 (TX/RX) -> HW-044 (MAX3232) -> RJ45
 * - Telnet-мост на порту 23, корректный разбор IAC, экранирование 0xFF
 * - Буфер истории 8 КБ: то, что железка напечатала ДО подключения
 *   клиента, не теряется и отдаётся сразу при коннекте
 * - BREAK: по IAC BRK из telnet-клиента (PuTTY: Special Command -> Break).
 *   Нужен для rommon/BootROM и сброса пароля. С кнопки убран — там теперь
 *   отключение зависшего клиента, которое требуется чаще
 * - Смена скорости БЕЗ перезагрузки ESP: кнопкой по кругу или в /settings.
 *   Прошивка вообще не вызывает ESP.restart() — каждая перезагрузка
 *   отправляет бутлоадерный мусор в консоль подопытного устройства
 * - Новый telnet-клиент вытесняет старого (роуминг по стойке рвёт TCP,
 *   а отказ в подключении заблокировал бы вас же до перезагрузки)
 * - AP поднята всегда — это гарантированный вход. STA в приоритете: если
 *   домашняя сеть задана, прошивка подключается к ней, а не найдя за 25 с,
 *   гасит радио STA и работает только точкой доступа, повторяя попытку
 *   раз в 5 минут (но не посреди telnet-сессии — см. staTick)
 * - mDNS: esp-console.local (captive portal выключен, см. CAPTIVE_PORTAL)
 * - Веб-интерфейс /settings и OTA /update за Basic Auth с блокировкой
 *   после 5 неудачных попыток
 * - /log: выгрузка буфера истории файлом — готовый протокол работ
 *   по железке. За авторизацией: в консоль уходят набранные пароли
 * - OLED 128x32 (или 64, см. SCREEN_HEIGHT): метки AP/STA/TLNT, которые
 *   подсвечиваются по активности, иконка батареи с уровнем заряда, КРУПНО
 *   адрес (двойная высота) и скорость. Поворот на 180 градусов включается
 *   в /settings. Гаснет через 60 с простоя, будится кнопкой или клиентом
 *
 * ========================= СХЕМА =========================
 *  D1 mini TX (GPIO1)  -> HW-044 TXD
 *  D1 mini RX (GPIO3)  -> HW-044 RXD
 *  D1 mini 3V3         -> HW-044 VCC
 *  D1 mini G           -> HW-044 GND
 *
 *  D1 mini D1 (GPIO5, SCL) -> OLED SCL
 *  D1 mini D2 (GPIO4, SDA) -> OLED SDA
 *  D1 mini 3V3             -> OLED VCC
 *  D1 mini G               -> OLED GND
 *
 *  Кнопка (ОПЦИОНАЛЬНО, без неё всё работает — пин подтянут внутрь):
 *  D1 mini D5 (GPIO14) -- тактовая кнопка -- GND
 *    короткое нажатие : экран погашен -> разбудить, иначе -> следующая скорость
 *    удержание 1 сек  : отключить telnet-клиента
 *  После пробуждения экрана кнопка не реагирует 0.7 с — чтобы двойной
 *  клик не проскочил в смену скорости
 *
 *  Батарея (ОБЯЗАТЕЛЬНО через делитель, НЕ на A0 напрямую!):
 *  Batt+ --[R1 100к]-- A0 --[R2 100к]-- GND
 *
 * ===================== ВАЖНЫЕ НЮАНСЫ =====================
 * 1. Serial (UART0) постоянно занят под консольный мост — в рабочем
 *    режиме туда ничего, кроме трафика консоли, не пишем. Для отладки
 *    при сборке (без подключенного HW-044) поставь DEBUG_SERIAL 1.
 * 2. Настройки этой версии несовместимы с v1.0: структура в EEPROM
 *    получила поле версии, при первом старте применятся значения
 *    по умолчанию. Пароль AP и PIN придётся задать заново.
 * 3. OLED-библиотека не умеет кириллицу штатным шрифтом — статус
 *    выведен на английском. Для кириллицы нужен переход на u8g2.
 * 4. Порядок сборки: прошей по USB БЕЗ подключенного HW-044 (иначе
 *    CH340 и MAX3232 спорят за RX), проверь WiFi/OLED, потом паяй
 *    модуль на TX/RX. Дальше — только OTA через /update.
 * 5. Basic Auth идёт по обычному HTTP (не HTTPS) — от случайного
 *    доступа защищает, от перехвата в общей сети — нет.
 * 6. HISTORY_SIZE ест статическую RAM. Если на /settings свободной
 *    памяти окажется меньше ~12 КБ — уменьши до 4096.
 *
 * ==================== НУЖНЫЕ БИБЛИОТЕКИ ====================
 * Через Library Manager: "Adafruit SSD1306", "Adafruit GFX Library"
 * (подтянет зависимость Adafruit BusIO). Остальное уже входит в
 * пакет плат ESP8266.
 */

// ---------- конфигурация сборки ----------
// Captive portal выключен намеренно. Всплывающее окно "Вход в сеть" на
// Android — это урезанный WebView, а не браузер: диалог Basic Auth он
// показывать не умеет и на ответ 401 от /settings выдаёт
// ERR_HTTP_RESPONSE_CODE_FAILURE. Пока авторизация построена на Basic
// Auth, окно только мешает. Без DNS-перехвата телефон просто пометит
// сеть как "без доступа в интернет" и оставит мобильный трафик себе.
// Ставить 1 имеет смысл после перехода на форму входа с сессионной кукой.
#define CAPTIVE_PORTAL 0

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#if CAPTIVE_PORTAL
  #include <DNSServer.h>
#endif
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define FW_VERSION "1.6.2"

// ---------- отладка (только на этапе сборки, без HW-044 на TX/RX) ----------
#define DEBUG_SERIAL 0   // поставь 1 для первой прошивки/проверки
#if DEBUG_SERIAL
  #define DBG(x) Serial.println(x)
#else
  #define DBG(x)
#endif

// На случай сборки под "generic ESP8266" вместо D1 mini
#ifndef D1
  #define D1 5
#endif
#ifndef D2
  #define D2 4
#endif
#ifndef D5
  #define D5 14
#endif

static inline size_t smin(size_t a, size_t b) { return a < b ? a : b; }

// ---------- пины ----------
#define PIN_BUTTON  D5   // GPIO14, кнопка на GND; не распаяна -> всегда HIGH
#define PIN_UART_TX 1    // GPIO1, временно отцепляется от UART0 ради BREAK

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32   // 32 или 64 — разметка экрана переключается автоматически
// Модули SSD1306 встречаются и на 0x3C, и на 0x3D — адрес подбираем сами.
const uint8_t OLED_ADDRS[] = {0x3C, 0x3D};
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool    oledOk   = false;
uint8_t oledAddr = 0;

// Adafruit_SSD1306::begin() в 2.5.x для I2C не проверяет ACK и возвращает
// true даже на пустой шине — как признак наличия экрана он бесполезен.
// Поэтому пробуем адрес сами: без устройства линия подтянута внутренним
// резистором в HIGH, это NACK, и endTransmission() вернёт не 0.
bool i2cDetect(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

#define SCREEN_TIMEOUT 60000UL
bool     screenOn = true;
uint32_t screenIdleSince = 0;

// ---------- скорости консоли ----------
// Единый источник правды: и для веб-формы, и для валидации POST, и для
// перебора кнопкой. Раньше POST принимал из формы любое число, и baud=1
// делал устройство неработоспособным до перепрошивки по USB.
const uint32_t BAUD_RATES[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
const uint8_t  BAUD_COUNT   = sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]);

bool baudIsValid(uint32_t b) {
  for (uint8_t i = 0; i < BAUD_COUNT; i++) if (BAUD_RATES[i] == b) return true;
  return false;
}
uint8_t baudIndex(uint32_t b) {
  for (uint8_t i = 0; i < BAUD_COUNT; i++) if (BAUD_RATES[i] == b) return i;
  return 3; // 9600
}

// ---------- настройки (EEPROM) ----------
#define SETTINGS_MAGIC   0xA9
#define SETTINGS_VERSION 2

struct Settings {
  uint8_t  magic;
  uint8_t  version;     // без него добавление поля после OTA читалось бы мусором
  char     apSsid[32];
  char     apPass[64];
  char     staSsid[32];
  char     staPass[64];
  uint32_t baud;
  char     pin[9];
  // Взято из бывшего reserved[16]: размер и смещения полей не изменились,
  // поэтому SETTINGS_VERSION поднимать не нужно и настройки переживут
  // обновление. В старых образах здесь ноль, то есть поворот выключен.
  uint8_t  flipScreen;   // 0 — обычная ориентация, 1 — поворот на 180°
  uint8_t  reserved[15];
};
const int EEPROM_SIZE = 256;
Settings settings;

// Отложенная запись: перебор скорости кнопкой — до 8 нажатий подряд,
// и каждое не должно стирать сектор flash.
bool     settingsDirty = false;
uint32_t settingsDirtyAt = 0;

uint32_t currentSerialBaud() {
  return DEBUG_SERIAL ? 115200UL : settings.baud;
}

void settingsDefaults() {
  memset(&settings, 0, sizeof(settings));
  settings.magic   = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  strcpy(settings.apSsid, "ESP-Console");
  strcpy(settings.apPass, "console123"); // сменить в /settings при первом входе
  strcpy(settings.pin,    "1234");       // сменить в /settings при первом входе
  settings.baud = 9600;
}

void saveSettingsNow() {
  EEPROM.put(0, settings);
  EEPROM.commit();
  settingsDirty = false;
}

void markSettingsDirty() {
  settingsDirty = true;
  settingsDirtyAt = millis();
}

void flushSettings() {
  if (settingsDirty && millis() - settingsDirtyAt > 5000) saveSettingsNow();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, settings);
  if (settings.magic != SETTINGS_MAGIC || settings.version != SETTINGS_VERSION) {
    settingsDefaults();
    saveSettingsNow();
  }
  if (!baudIsValid(settings.baud)) settings.baud = 9600;
}

// ---------- батарея ----------
// ПОДСТРОЙ под свою плату: измерь реальное напряжение мультиметром и
// подгони ADC_FULL_SCALE так, чтобы цифра на экране совпала.
const float DIVIDER_RATIO  = 2.0;
const float ADC_FULL_SCALE = 3.3;
const float BATT_LOW_V     = 3.40;  // порог предупреждения на экране

float battV = 0;

// Замер без delay(): один отсчёт за проход loop, скользящее среднее.
// Прежний вариант (8 замеров через delay(2)) блокировал мост на 16 мс
// каждую секунду — на 115200 это гарантированная потеря байт консоли.
void sampleBattery() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 50) return;
  last = now;
  float v = (analogRead(A0) / 1023.0f) * ADC_FULL_SCALE * DIVIDER_RATIO;
  battV = (battV <= 0.01f) ? v : (battV * 0.9f + v * 0.1f);
}

// Кусочно-линейная кривая Li-ion: линейная аппроксимация по напряжению
// врёт в середине разряда на десятки процентов.
struct BattPoint { float v; uint8_t pct; };
const BattPoint BATT_CURVE[] = {
  {3.30f, 0}, {3.50f, 10}, {3.65f, 25}, {3.75f, 45},
  {3.85f, 60}, {3.95f, 75}, {4.05f, 90}, {4.20f, 100}
};
const uint8_t BATT_POINTS = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);

uint8_t batteryPercent(float v) {
  if (v <= BATT_CURVE[0].v) return 0;
  if (v >= BATT_CURVE[BATT_POINTS - 1].v) return 100;
  for (uint8_t i = 1; i < BATT_POINTS; i++) {
    if (v < BATT_CURVE[i].v) {
      float span = BATT_CURVE[i].v - BATT_CURVE[i - 1].v;
      float k = (v - BATT_CURVE[i - 1].v) / span;
      return BATT_CURVE[i - 1].pct + (uint8_t)(k * (BATT_CURVE[i].pct - BATT_CURVE[i - 1].pct));
    }
  }
  return 100;
}

// ---------- экран: питание ----------
void wakeScreen() {
  screenIdleSince = millis();
  if (oledOk && !screenOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    screenOn = true;
  }
}

void screenPowerTick() {
  if (oledOk && screenOn && millis() - screenIdleSince > SCREEN_TIMEOUT) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    screenOn = false;
  }
}

// ---------- буфер истории консоли ----------
// Классический сценарий: воткнул консоль, подал питание на железку,
// достал телефон, подключился — а весь POST и вывод загрузчика уже
// уехали в никуда. Всё, что приходит из UART, складывается сюда;
// telnet-клиент читает отсюда же, поэтому "живой" и "исторический"
// трафик идут по одному пути и не могут перепутаться местами.
#define HISTORY_SIZE 8192
uint8_t  history[HISTORY_SIZE];
uint32_t histTotal = 0;   // сколько байт всего прошло через UART
uint32_t histRead  = 0;   // позиция telnet-читателя

// histTotal/histRead монотонны; все сравнения идут через беззнаковую
// разность, которая корректна и при переполнении uint32_t.
void historyPush(const uint8_t* d, size_t n) {
  size_t pos = histTotal % HISTORY_SIZE;
  for (size_t i = 0; i < n; i++) {
    history[pos] = d[i];
    if (++pos == HISTORY_SIZE) pos = 0;
  }
  histTotal += n;
  if (histTotal - histRead > HISTORY_SIZE) histRead = histTotal - HISTORY_SIZE;
}

// ---------- BREAK ----------
// Сброс пароля на Cisco (rommon), вход в BootROM на Huawei, прерывание
// автозагрузки на большинстве МСЭ — всё это делается сигналом BREAK.
// TTL LOW на TX -> на выходе MAX3232 положительное напряжение (SPACE),
// удержание SPACE дольше длительности символа и есть break condition.
// Короткое уведомление в нижней строке экрана: одно место вместо
// отдельного флага на каждое событие.
#define NOTICE_BREAK 1
#define NOTICE_KICK  2
uint8_t  noticeKind  = 0;
uint32_t noticeUntil = 0;

void showNotice(uint8_t kind, uint16_t ms = 1500) {
  noticeKind  = kind;
  noticeUntil = millis() + ms;
  wakeScreen();
}
uint8_t activeNotice() {
  return (noticeKind && (int32_t)(millis() - noticeUntil) < 0) ? noticeKind : 0;
}

// Объявления: определены ниже по тексту, а вызываются выше.
void serialToHistory();
void applyScreenRotation();

void sendBreak(uint16_t ms) {
  Serial.flush();
  pinMode(PIN_UART_TX, OUTPUT);      // GPIO1 отцепляется от UART0
  digitalWrite(PIN_UART_TX, LOW);
  delay(ms);
  digitalWrite(PIN_UART_TX, HIGH);
  // Железка отвечает на break сразу, ещё пока он держится, и её первые
  // строки уже лежат в приёмном буфере UART. Serial.begin() ниже
  // пересоздаёт буфер пустым, поэтому забираем их ДО переинициализации —
  // иначе теряется ровно то, ради чего break и посылался.
  serialToHistory();
  Serial.begin(currentSerialBaud()); // begin() возвращает пин под UART0
  showNotice(NOTICE_BREAK);
  DBG("BREAK отправлен");
}

// ---------- telnet ----------
WiFiServer telnetServer(23);
WiFiClient telnetClient;

#define TELNET_IDLE_TIMEOUT 600000UL   // 10 минут
uint32_t telnetLastActivity = 0;

enum TnState { TN_DATA, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC };
TnState tnState = TN_DATA;

// 0xFF в потоке от железки клиент воспримет как IAC — по RFC 854 его
// надо удваивать. Иначе бинарный вывод ломает telnet-сессию.
void telnetWriteEscaped(const uint8_t* d, size_t n) {
  static uint8_t tx[1024];
  size_t k = 0;
  for (size_t i = 0; i < n && k < sizeof(tx) - 1; i++) {
    tx[k++] = d[i];
    if (d[i] == 255) tx[k++] = 255;
  }
  telnetClient.write(tx, k);
}

void telnetAccept() {
  if (!telnetServer.hasClient()) return;
  WiFiClient newClient = telnetServer.available();

  // Вытеснение вместо отказа: при переходе по стойке телефон теряет AP,
  // TCP остаётся полуоткрытым, и отказ новому клиенту заблокировал бы
  // доступ до перезагрузки ESP — то есть до мусора в чужой консоли.
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(F("\r\n[session taken over]\r\n"));
    telnetClient.stop();
  }
  telnetClient = newClient;
  telnetClient.setNoDelay(true);
  telnetClient.keepAlive();
  tnState = TN_DATA;
  telnetLastActivity = millis();

  const uint8_t negotiate[] = {255, 251, 1, 255, 251, 3}; // IAC WILL ECHO, WILL SGA
  telnetClient.write(negotiate, sizeof(negotiate));

  // отматываем читателя назад — отдадим накопленную историю
  uint32_t have = (histTotal < HISTORY_SIZE) ? histTotal : (uint32_t)HISTORY_SIZE;
  histRead = histTotal - have;

  wakeScreen();
  DBG("Telnet: клиент подключился");
}

void telnetToSerial() {
  if (!(telnetClient && telnetClient.connected())) return;
  // Условие availableForWrite() критично для вставки конфига из буфера
  // обмена: на 9600 несколько килобайт уходят в UART секунды, и слепой
  // Serial.write() блокировал бы весь loop на всё это время. Здесь
  // вместо блокировки просто перестаём читать из TCP — окно закрывается,
  // клиент притормаживает сам, ни один байт конфига не теряется.
  while (telnetClient.available() && Serial.availableForWrite() > 0) {
    int r = telnetClient.read();
    if (r < 0) break;
    uint8_t b = (uint8_t)r;
    telnetLastActivity = millis();

    // Конечный автомат вместо вложенных read(): прежний вариант вызывал
    // read() сразу после IAC и получал -1, если TCP-пакет закончился
    // ровно на 255 — следующий реальный байт данных при этом съедался.
    switch (tnState) {
      case TN_DATA:
        if (b == 255) tnState = TN_IAC;
        else Serial.write(b);
        break;

      case TN_IAC:
        if (b == 255) { Serial.write((uint8_t)255); tnState = TN_DATA; } // экранированный 0xFF
        else if (b == 250) tnState = TN_SB;                             // SB
        else if (b >= 251 && b <= 254) tnState = TN_OPT;                // WILL/WONT/DO/DONT
        else {
          if (b == 243) sendBreak(300);                    // BRK: PuTTY -> Special Command -> Break
          else if (b == 244) Serial.write((uint8_t)0x03);  // IP -> Ctrl+C
          else if (b == 246) telnetClient.print(F("\r\n[esp-console alive]\r\n")); // AYT
          tnState = TN_DATA;
        }
        break;

      case TN_OPT:
        tnState = TN_DATA;   // проглатываем код опции
        break;

      case TN_SB:
        if (b == 255) tnState = TN_SB_IAC;
        break;

      case TN_SB_IAC:
        tnState = (b == 240) ? TN_DATA : TN_SB;   // SE завершает подпереговоры
        break;
    }
  }
}

void serialToHistory() {
  static uint8_t buf[512];
  size_t n = 0;
  while (Serial.available() && n < sizeof(buf)) buf[n++] = (uint8_t)Serial.read();
  if (n) historyPush(buf, n);
}

void historyToTelnet() {
  if (!(telnetClient && telnetClient.connected())) return;
  while (histTotal != histRead) {
    size_t room = telnetClient.availableForWrite();
    if (room < 64) break;                        // окно закрыто, доберём в следующий проход
    size_t pending = (size_t)(histTotal - histRead);
    size_t pos     = histRead % HISTORY_SIZE;
    size_t chunk   = smin(pending, (size_t)512);
    chunk = smin(chunk, room / 2);                // запас на удвоение 0xFF
    chunk = smin(chunk, HISTORY_SIZE - pos);      // не перескакиваем через край кольца
    if (chunk == 0) break;
    telnetWriteEscaped(&history[pos], chunk);
    histRead += chunk;
    telnetLastActivity = millis();
  }
}

void telnetTick() {
  telnetAccept();
  telnetToSerial();
  serialToHistory();
  historyToTelnet();

  if (telnetClient) {
    if (!telnetClient.connected()) {
      telnetClient.stop();
      wakeScreen();
    } else if (millis() - telnetLastActivity > TELNET_IDLE_TIMEOUT) {
      telnetClient.print(F("\r\n[idle timeout]\r\n"));
      telnetClient.stop();
      wakeScreen();
      DBG("Telnet: сессия закрыта по таймауту");
    }
  }
}

// Принудительный сброс сессии с кнопки. Нужен, когда клиент завис в
// полуоткрытом TCP и вы хотите освободить порт, не дожидаясь таймаута.
void kickTelnet() {
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(F("\r\n[disconnected by device]\r\n"));
    telnetClient.stop();
    DBG("Telnet: клиент отключён кнопкой");
  }
  showNotice(NOTICE_KICK);
}

// ---------- скорость ----------
void applyBaud(uint32_t b) {
  if (!baudIsValid(b) || b == settings.baud) return;
  settings.baud = b;
  Serial.flush();
  Serial.updateBaudRate(currentSerialBaud());  // без ESP.restart(): перезагрузка
  markSettingsDirty();                         // ESP отправила бы мусор в консоль
  wakeScreen();
}

void nextBaud() {
  applyBaud(BAUD_RATES[(baudIndex(settings.baud) + 1) % BAUD_COUNT]);
}

// ---------- кнопка ----------
#define BTN_DEBOUNCE  30
#define BTN_LONG_MS   1000
#define BTN_WAKE_LOCK 700    // сколько игнорируем нажатия после пробуждения экрана
uint32_t btnLockUntil = 0;
bool     btnReleased = true;
uint32_t btnDownAt = 0;
bool     btnLongDone = false;

void handleButton() {
  bool up = digitalRead(PIN_BUTTON);   // HIGH = отпущена (или не распаяна)
  uint32_t t = millis();

  if (btnReleased && !up) {
    btnReleased = false;
    btnDownAt = t;
    btnLongDone = false;
  } else if (!btnReleased && !up) {
    if (!btnLongDone && t - btnDownAt >= BTN_LONG_MS) {
      btnLongDone = true;
      kickTelnet();
    }
  } else if (!btnReleased && up) {
    btnReleased = true;
    if (!btnLongDone && t - btnDownAt >= BTN_DEBOUNCE) {
      // Первое нажатие при погашенном экране только будит — иначе
      // "посмотреть скорость" молча переключало бы её посреди сессии.
      if (!screenOn) {
        wakeScreen();
        // Пауза после пробуждения: без неё второе нажатие в темпе двойного
        // клика проскакивает дальше и незаметно меняет скорость.
        btnLockUntil = t + BTN_WAKE_LOCK;
      } else if ((int32_t)(t - btnLockUntil) >= 0) {
        nextBaud();
      }
    }
  }
}

// ---------- веб ----------
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
#if CAPTIVE_PORTAL
  DNSServer dnsServer;
#endif
const char* AUTH_USER = "admin";
const char* MDNS_HOST = "esp-console";

uint8_t  authFails = 0;
bool     authLocked = false;
uint32_t authLockAt = 0;
#define AUTH_LOCK_MS 60000UL

bool checkAuth() {
  if (authLocked) {
    if (millis() - authLockAt < AUTH_LOCK_MS) {
      server.send(429, "text/plain", "Too many attempts. Wait 60s.");
      return false;
    }
    authLocked = false;
  }
  if (!server.authenticate(AUTH_USER, settings.pin)) {
    // Задержки здесь намеренно нет: delay() блокировал бы консольный
    // мост. Перебор ограничиваем блокировкой, а не засыпанием.
    if (++authFails >= 5) { authLocked = true; authLockAt = millis(); authFails = 0; }
    server.requestAuthentication();
    return false;
  }
  authFails = 0;
  return true;
}

String htmlEscape(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    if (*p == '\'')      out += "&#39;";
    else if (*p == '"')  out += "&quot;";
    else if (*p == '<')  out += "&lt;";
    else if (*p == '>')  out += "&gt;";
    else if (*p == '&')  out += "&amp;";
    else out += *p;
  }
  return out;
}

const char PAGE_HEADER[] PROGMEM =
  "<!DOCTYPE html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>ESP Console</title><style>"
  "body{background:#111;color:#ddd;font-family:monospace;margin:0;padding:14px}"
  "input,select,button{font-family:monospace;font-size:14px;padding:6px;margin:2px 0}"
  "input[type=text],input[type=password],select{width:100%;max-width:320px;box-sizing:border-box}"
  "fieldset{border:1px solid #444;margin-bottom:14px}"
  "legend{color:#6cf}label{display:block;margin-top:8px}"
  "button{padding:8px 16px;margin-top:12px}"
  "a.btn{display:inline-block;padding:8px 16px;border:1px solid #6cf;color:#6cf;"
  "text-decoration:none;margin-top:6px}"
  ".status{color:#0f0}.status.off{color:#f66}"
  ".chk{width:auto;margin-right:6px}"
  ".sys{color:#888;font-size:12px;margin-top:18px}"
  "</style></head><body>";

// Страница отдаётся кусками: собирать её целиком в String — это ~3.5 КБ
// плюс столько же на копию внутри send(), на фоне 8 КБ истории лишнее.
String chunkBuf;
void emitFlush() { if (chunkBuf.length()) { server.sendContent(chunkBuf); chunkBuf = ""; } }
void emit(const __FlashStringHelper* s) { chunkBuf += s; if (chunkBuf.length() > 900) emitFlush(); }
void emit(const String& s)              { chunkBuf += s; if (chunkBuf.length() > 900) emitFlush(); }
void emit(uint32_t v)                   { chunkBuf += v; if (chunkBuf.length() > 900) emitFlush(); }

String uptimeStr() {
  uint32_t s = millis() / 1000;
  char b[28];
  snprintf(b, sizeof(b), "%lud %02lu:%02lu:%02lu",
           (unsigned long)(s / 86400), (unsigned long)((s / 3600) % 24),
           (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  return String(b);
}

void handleRoot() {
  server.sendHeader("Location", "/settings", true);
  server.send(302, "text/plain", "");
}

// Ловит опечатки в пути на самом устройстве. При CAPTIVE_PORTAL 1 сюда же
// приходят перехваченные через DNS проверки связи — тогда этот редирект и
// открывает страницу настроек автоматически.
void handleNotFound() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/settings", true);
  server.send(302, "text/plain", "");
}

void handleSettingsGet() {
  if (!checkAuth()) return;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  chunkBuf = "";
  chunkBuf.reserve(1100);

  emit(FPSTR(PAGE_HEADER));
  emit(F("<h2>ESP Console — настройки</h2>"));

  emit(F("<p>Точка доступа: <b>"));
  emit(htmlEscape(settings.apSsid));
  emit(F("</b>, IP: <b>"));
  emit(WiFi.softAPIP().toString());
  emit(F("</b><br>Имя в сети: <b>"));
  emit(String(MDNS_HOST) + ".local");
  emit(F("</b></p><p>Домашний WiFi: "));
  if (strlen(settings.staSsid) == 0) {
    emit(F("<span class='status off'>не настроен</span>"));
  } else if (WiFi.status() == WL_CONNECTED) {
    emit(F("<span class='status'>подключено</span>, IP: <b>"));
    emit(WiFi.localIP().toString());
    emit(F("</b>"));
  } else {
    emit(F("<span class='status off'>не подключено</span>"));
  }
  emit(F("</p>"));

  emit(F("<form method='POST' action='/settings'>"
    "<fieldset><legend>Консоль</legend>"
    "<label>Скорость (применяется сразу, без перезагрузки)</label>"
    "<select name='baud'>"));
  for (uint8_t i = 0; i < BAUD_COUNT; i++) {
    emit(F("<option value='")); emit(BAUD_RATES[i]); emit(F("'"));
    if (BAUD_RATES[i] == settings.baud) emit(F(" selected"));
    emit(F(">")); emit(BAUD_RATES[i]); emit(F("</option>"));
  }
  emit(F("</select></fieldset>"));

  emit(F("<fieldset><legend>Экран</legend>"
    "<label><input class='chk' type='checkbox' name='flip' value='1'"));
  if (settings.flipScreen) emit(F(" checked"));
  emit(F(">Перевернуть на 180&deg; (применяется сразу)</label></fieldset>"));

  // Пароли больше не подставляются в value: страница с открытым паролем
  // от домашнего WiFi — плохая идея на устройстве с обычным HTTP.
  emit(F("<fieldset><legend>Точка доступа устройства</legend>"
    "<label>SSID</label><input type='text' name='ap_ssid' maxlength='31' value='"));
  emit(htmlEscape(settings.apSsid));
  emit(F("'><label>Пароль (пусто — не менять, мин. 8 символов)</label>"
    "<input type='password' name='ap_pass' maxlength='63' placeholder='"));
  emit(strlen(settings.apPass) ? F("не менять") : F("сеть открыта"));
  emit(F("'><label><input class='chk' type='checkbox' name='ap_open' value='1'>"
    "Сделать сеть открытой (без пароля)</label></fieldset>"));

  emit(F("<fieldset><legend>Домашний WiFi (опционально)</legend>"
    "<label>SSID (пусто — не подключаться)</label>"
    "<input type='text' name='sta_ssid' maxlength='31' value='"));
  emit(htmlEscape(settings.staSsid));
  emit(F("'><label>Пароль (пусто — не менять)</label>"
    "<input type='password' name='sta_pass' maxlength='63' placeholder='"));
  emit(strlen(settings.staPass) ? F("не менять") : F("сеть открыта"));
  emit(F("'><label><input class='chk' type='checkbox' name='sta_open' value='1'>"
    "Сеть открытая (без пароля)</label></fieldset>"));

  emit(F("<fieldset><legend>PIN-код веб-интерфейса</legend>"
    "<label>Новый PIN (4-8 символов, пусто — не менять)</label>"
    "<input type='password' name='new_pin' maxlength='8'>"
    "<label>Повтор нового PIN</label>"
    "<input type='password' name='new_pin2' maxlength='8'></fieldset>"));

  emit(F("<button type='submit'>Сохранить</button></form>"));
  emit(F("<p><a class='btn' href='/log'>Скачать лог консоли</a> "
         "<a class='btn' href='/update'>OTA-обновление прошивки</a></p>"));

  emit(F("<p class='sys'>Прошивка " FW_VERSION " &middot; uptime "));
  emit(uptimeStr());
  emit(F(" &middot; свободно RAM "));
  emit((uint32_t)ESP.getFreeHeap());
  emit(F(" Б &middot; батарея "));
  emit((uint32_t)batteryPercent(battV));
  emit(F("%</p></body></html>"));

  emitFlush();
  server.sendContent("");
}

// Выгрузка буфера истории файлом. За авторизацией не только ради приличия:
// в консоль во время настройки уходят пароли, которые вы набираете руками,
// и всё это лежит в буфере.
void handleLog() {
  if (!checkAuth()) return;

  // Снимок границы: консоль продолжает писать, пока мы отдаём файл, и без
  // фиксации конца отдали бы больше, чем объявили в Content-Length.
  // Позиция своя — histRead принадлежит telnet-читателю, его не трогаем.
  uint32_t end  = histTotal;
  uint32_t have = (end < HISTORY_SIZE) ? end : (uint32_t)HISTORY_SIZE;
  uint32_t pos  = end - have;

  server.sendHeader("Content-Disposition", "attachment; filename=\"console.log\"");
  server.setContentLength(have);
  server.send(200, "text/plain", "");

  while (pos != end) {
    size_t off   = pos % HISTORY_SIZE;
    size_t chunk = smin((size_t)(end - pos), HISTORY_SIZE - off);  // не через край кольца
    chunk = smin(chunk, (size_t)512);
    server.sendContent((const char*)&history[off], chunk);
    pos += chunk;
  }
}

// ---------- WiFi ----------
// Применение конфигурации AP отложено: сначала браузер должен получить
// ответ, и только потом точка доступа переподнимается.
bool     pendingApReconfig = false;
uint32_t pendingApAt = 0;

// ---------- STA: приоритет домашней сети, откат на AP ----------
// Точка доступа поднята всегда — это единственный гарантированный вход,
// её мы не выключаем ни при каких обстоятельствах. STA поднимается при
// первой возможности; если сеть не нашлась за отведённое окно, радио STA
// выключается совсем (чтобы не сканировать эфир впустую за счёт батареи),
// и попытка повторяется раз в STA_RETRY_INTERVAL.
enum StaState { STA_DISABLED, STA_TRYING, STA_UP, STA_FALLBACK };
StaState staState   = STA_DISABLED;
uint32_t staStateAt = 0;

#define STA_CONNECT_WINDOW 25000UL    // сколько ждём ассоциации
#define STA_RETRY_INTERVAL 300000UL   // пауза между попытками, 5 минут

void staBeginAttempt() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(settings.staSsid, strlen(settings.staPass) ? settings.staPass : nullptr);
  staState   = STA_TRYING;
  staStateAt = millis();
}

void staStop() {
  WiFi.disconnect(true);   // выключает радио STA; persistent(false) — флеш не трогается
  WiFi.mode(WIFI_AP);
  staState   = STA_DISABLED;
  staStateAt = millis();
}

void staTick() {
  if (strlen(settings.staSsid) == 0) {
    if (staState != STA_DISABLED) staStop();
    return;
  }

  uint32_t now = millis();
  switch (staState) {
    case STA_DISABLED:                 // сеть задана, STA ещё не поднимался
      staBeginAttempt();
      break;

    case STA_TRYING:
      if (WiFi.status() == WL_CONNECTED) {
        staState = STA_UP;  staStateAt = now;  wakeScreen();
      } else if (now - staStateAt > STA_CONNECT_WINDOW) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);            // откат: работаем только точкой доступа
        staState = STA_FALLBACK;  staStateAt = now;  wakeScreen();
      }
      break;

    case STA_UP:
      if (WiFi.status() != WL_CONNECTED) {   // связь пропала — пробуем снова
        staState = STA_TRYING;  staStateAt = now;  wakeScreen();
      }
      break;

    case STA_FALLBACK:
      if (now - staStateAt < STA_RETRY_INTERVAL) break;
      // Успешное подключение STA переводит точку доступа на канал домашней
      // сети и рвёт всех её клиентов. Посреди работы с консолью это
      // недопустимо, поэтому попытку откладываем до конца сессии.
      if (telnetClient && telnetClient.connected()) { staStateAt = now; break; }
      staBeginAttempt();
      break;
  }
}

// Настройки изменились — пересобрать состояние с нуля, дальше решит staTick().
void applyStaConfig() { staStop(); }

void applyApConfig() {
  WiFi.softAP(settings.apSsid, strlen(settings.apPass) ? settings.apPass : nullptr);
}

void setupWifi() {
  // persistent(false): иначе core пишет учётки во flash при каждом begin()
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_AP);          // точка доступа поднимается первой и всегда
  WiFi.setOutputPower(12.0);   // дистанция "рука — сумка", 20.5 dBm ни к чему
  applyApConfig();
  staState = STA_DISABLED;     // staTick() поднимет STA, если он задан
}

void handleSettingsPost() {
  if (!checkAuth()) return;

  String apSsid  = server.arg("ap_ssid");
  String apPass  = server.arg("ap_pass");
  bool   apOpen  = server.hasArg("ap_open");
  String staSsid = server.arg("sta_ssid");
  String staPass = server.arg("sta_pass");
  bool   staOpen = server.hasArg("sta_open");
  String baudStr = server.arg("baud");
  String newPin  = server.arg("new_pin");
  String newPin2 = server.arg("new_pin2");

  if (apSsid.length() == 0 || apSsid.length() > 31) {
    server.send(400, "text/plain", "SSID точки доступа: 1-31 символ"); return;
  }
  if (!apOpen && apPass.length() > 0 && apPass.length() < 8) {
    server.send(400, "text/plain", "Пароль точки доступа: от 8 символов"); return;
  }
  if (!staOpen && staSsid.length() > 0 && staPass.length() > 0 && staPass.length() < 8) {
    server.send(400, "text/plain", "Пароль домашнего WiFi: от 8 символов"); return;
  }
  uint32_t newBaud = strtoul(baudStr.c_str(), nullptr, 10);
  if (!baudIsValid(newBaud)) {
    server.send(400, "text/plain", "Недопустимая скорость консоли"); return;
  }
  if (newPin.length() > 0) {
    if (newPin.length() < 4) {
      server.send(400, "text/plain", "PIN должен быть от 4 символов"); return;
    }
    if (newPin != newPin2) {
      server.send(400, "text/plain", "PIN и повтор не совпадают"); return;
    }
  }

  bool apChanged  = (apSsid != settings.apSsid) || apOpen || apPass.length() > 0;
  bool staChanged = (staSsid != settings.staSsid) || staOpen || staPass.length() > 0;

  strncpy(settings.apSsid, apSsid.c_str(), sizeof(settings.apSsid) - 1);
  settings.apSsid[sizeof(settings.apSsid) - 1] = 0;
  if (apOpen) settings.apPass[0] = 0;
  else if (apPass.length() > 0) {
    strncpy(settings.apPass, apPass.c_str(), sizeof(settings.apPass) - 1);
    settings.apPass[sizeof(settings.apPass) - 1] = 0;
  }

  strncpy(settings.staSsid, staSsid.c_str(), sizeof(settings.staSsid) - 1);
  settings.staSsid[sizeof(settings.staSsid) - 1] = 0;
  if (staOpen) settings.staPass[0] = 0;
  else if (staPass.length() > 0) {
    strncpy(settings.staPass, staPass.c_str(), sizeof(settings.staPass) - 1);
    settings.staPass[sizeof(settings.staPass) - 1] = 0;
  }

  if (newPin.length() > 0) {
    strncpy(settings.pin, newPin.c_str(), sizeof(settings.pin) - 1);
    settings.pin[sizeof(settings.pin) - 1] = 0;
    // OTA-эндпоинт держит копию учёток — обновляем, иначе до перезагрузки
    // /update продолжал бы принимать старый PIN.
    httpUpdater.updateCredentials(AUTH_USER, settings.pin);
  }

  uint8_t flip = server.hasArg("flip") ? 1 : 0;
  if (settings.flipScreen != flip) {
    settings.flipScreen = flip;
    applyScreenRotation();
  }

  applyBaud(newBaud);
  saveSettingsNow();

  String msg = F("<html><head><meta charset='utf-8'></head>"
    "<body style='font-family:monospace;background:#111;color:#ddd;padding:14px'>"
    "Сохранено. Перезагрузка не требуется.");
  if (apChanged) msg += F("<br><br>Точка доступа переподнимается — переподключитесь к WiFi.");
  msg += F("<br><br><a style='color:#6cf' href='/settings'>Назад</a></body></html>");
  server.send(200, "text/html", msg);

  if (staChanged) applyStaConfig();
  if (apChanged) { pendingApReconfig = true; pendingApAt = millis(); }
  wakeScreen();
}

// ---------- OLED ----------
// Английский текст намеренно: штатный шрифт Adafruit_GFX не знает кириллицу.
struct ScreenState {
  uint32_t apIp;
  uint32_t staIp;
  uint32_t baud;
  uint8_t  staMode;   // 0 выкл, 1 поиск, 2 подключено, 3 сеть не найдена
  uint8_t  pct;
  uint8_t  flags;     // bit0 telnet, bit1 клиенты AP, bit2 низкий заряд, bit3 STA поднят
  uint8_t  notice;    // 0, NOTICE_BREAK или NOTICE_KICK
};                    // 3×uint32 + 4×uint8 = ровно 16 байт, дыр для memcmp нет
ScreenState prevScreen;
bool prevScreenValid = false;

void applyScreenRotation() {
  if (oledOk) display.setRotation(settings.flipScreen ? 2 : 0);
  prevScreenValid = false;   // разметка сменилась — перерисовать принудительно
}

// ---------- заставка ----------
// Заливка целиком (заодно видно все пиксели матрицы), затем бегущая строка.
// Крутится из loop(), а не задержками в setup(): железка может печатать
// прямо во время старта, и терять её вывод ради анимации незачем.
#define SPLASH_FILL_MS  700
#define SPLASH_STEP_MS  25
#define SPLASH_STEP_PX  4
#define SPLASH_TEXT_W   (11 * 12)   // "ESP CONSOLE" шрифтом size 2

enum SplashState { SPLASH_FILL, SPLASH_SCROLL, SPLASH_OFF };
SplashState splashState = SPLASH_OFF;
uint32_t    splashAt = 0;
int16_t     splashX  = SCREEN_WIDTH;

void splashTick() {
  if (splashState == SPLASH_OFF) return;
  if (!oledOk) { splashState = SPLASH_OFF; return; }

  uint32_t now = millis();

  if (splashState == SPLASH_FILL) {
    // splashAt == 0 значит "ещё не рисовали". Заливку выводим здесь, а не
    // в setup(), просто чтобы она попадала на экран после полной
    // инициализации и её длительность не съедалась поднятием WiFi.
    if (splashAt == 0) {
      display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
      display.display();
      splashAt = now ? now : 1;
      return;
    }
    if (now - splashAt < SPLASH_FILL_MS) return;
    splashState = SPLASH_SCROLL;
    splashX = SCREEN_WIDTH;
    splashAt = now;
    return;
  }

  if (now - splashAt < SPLASH_STEP_MS) return;
  splashAt = now;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(splashX, (SCREEN_HEIGHT - 16) / 2);
  display.print(F("ESP CONSOLE"));
  display.setTextSize(1);
  display.display();

  splashX -= SPLASH_STEP_PX;
  if (splashX < -SPLASH_TEXT_W) {     // строка ушла за левый край
    splashState = SPLASH_OFF;
    prevScreenValid = false;          // вернуть обычную картинку
  }
}

// Правое выравнивание: ширина символа шрифта size 1 — 6 пикселей.
void printRight(const char* s, int16_t y, int16_t rightEdge = SCREEN_WIDTH) {
  display.setCursor(rightEdge - 6 * (int16_t)strlen(s), y);
  display.print(s);
}

// Короткая метка состояния: активна — подсвечена, текст инверсный.
// x — позиция ТЕКСТА; подложка выступает на BADGE_PAD влево и вправо.
// Поля обязательны: при радиусе 2 срезанные углы иначе съедали бы верхние
// пиксели букв вроде T и S, которые в строке 0 занимают всю ширину глифа.
// padTop поднимает подложку выше текста, когда над строкой есть зазор.
#define BADGE_PAD 2
void badge(int16_t x, int16_t y, const char* t, bool on, int16_t padTop = 0) {
  if (on) {
    int16_t w = 6 * (int16_t)strlen(t) + BADGE_PAD * 2;
    display.fillRoundRect(x - BADGE_PAD, y - padTop, w, 8, 2, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }
  display.setCursor(x, y);
  display.print(t);
  display.setTextColor(SSD1306_WHITE);
}

// Иконка скорости линии: пачка импульсов, как на осциллограмме UART.
// Занимает 11x7 пикселей.
void drawBaudIcon(int16_t x, int16_t y) {
  display.drawFastHLine(x,     y + 6, 3, SSD1306_WHITE);
  display.drawFastVLine(x + 2, y,     7, SSD1306_WHITE);
  display.drawFastHLine(x + 3, y,     3, SSD1306_WHITE);
  display.drawFastVLine(x + 5, y,     7, SSD1306_WHITE);
  display.drawFastHLine(x + 6, y + 6, 3, SSD1306_WHITE);
  display.drawFastVLine(x + 8, y,     7, SSD1306_WHITE);
  display.drawFastHLine(x + 9, y,     2, SSD1306_WHITE);
}

// Батарейка 15x7: корпус, контакт справа, заполнение по уровню заряда.
void drawBattery(int16_t x, int16_t y, uint8_t pct) {
  display.drawRect(x, y, 13, 7, SSD1306_WHITE);
  display.fillRect(x + 13, y + 2, 2, 3, SSD1306_WHITE);
  uint8_t w = (uint8_t)((uint16_t)pct * 11 / 100);
  if (w) display.fillRect(x + 1, y + 1, w, 5, SSD1306_WHITE);
}

void updateOled() {
  if (!oledOk || !screenOn || splashState != SPLASH_OFF) return;

  ScreenState s;
  memset(&s, 0, sizeof(s));
  s.apIp    = (uint32_t)WiFi.softAPIP();
  s.staIp   = (uint32_t)WiFi.localIP();
  s.baud    = settings.baud;
  s.staMode = (strlen(settings.staSsid) == 0) ? 0
            : (staState == STA_UP)            ? 2
            : (staState == STA_FALLBACK)      ? 3 : 1;
  s.pct     = batteryPercent(battV);
  s.notice  = activeNotice();
  if (telnetClient && telnetClient.connected())    s.flags |= 1;
  if (WiFi.softAPgetStationNum() > 0)              s.flags |= 2;
  if (battV > 0.5f && battV < BATT_LOW_V)          s.flags |= 4;
  if (s.staMode == 2)                              s.flags |= 8;

  // Полная перерисовка — это ~1 КБ по I2C, десятки миллисекунд с
  // заблокированным мостом. Раньше она шла безусловно раз в секунду.
  if (prevScreenValid && memcmp(&s, &prevScreen, sizeof(s)) == 0) return;
  prevScreen = s;
  prevScreenValid = true;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Скорость — крупно. При первичной настройке это главный вопрос
  // ("на чём я сейчас сижу?"), и лезть за ним в браузер неудобно.
  char baudBuf[8];
  snprintf(baudBuf, sizeof(baudBuf), "%lu", (unsigned long)settings.baud);

#if SCREEN_HEIGHT == 32
  // Три ряда по 8 / 16 / 8 пикселей — ровно 32.
  //
  //  AP STA TLNT          87% [==  ]   ряд меток и батарейка
  //  192.168.1.150                     адрес двойной высоты
  //  BR 115200                         скорость, либо инверсное уведомление
  //
  // Метки подсвечиваются, когда соответствующее состояние активно:
  // AP — к точке доступа кто-то подключён, STA — домашняя сеть поднята,
  // TLNT — открыта консольная сессия.
  // x — позиция текста, подложка шире на 2 px с каждой стороны, поэтому
  // AP начинается с 2: её подложка занимает 0..15, STA — 20..41.
  display.setTextSize(1);
  badge(2,  0, "AP",  s.flags & 2);
  badge(22, 0, "STA", s.flags & 8);

  char battBuf[8];
  if (s.flags & 4) strcpy(battBuf, "LOW");
  else             snprintf(battBuf, sizeof(battBuf), "%u%%", (unsigned)s.pct);
  printRight(battBuf, 0, 109);      // слева от иконки, та занимает 113..127
  drawBattery(113, 0, s.pct);

  // Полноразмерный size 2 не подходит: 13 символов по 12 px это 156 при
  // ширине 128. Двойная высота при обычной ширине даёт крупный адрес,
  // который влезает при любом IPv4.
  display.setTextSize(1, 2);
  display.setCursor(0, 9);   // 1 px отступа от метки: вплотную они слипаются
  if (s.flags & 8) display.print(WiFi.localIP());   // STA поднят — показываем его
  else             display.print(WiFi.softAPIP());  // иначе адрес точки доступа
  display.setTextSize(1);

  // Нижняя строка сдвинута на 25: два пикселя зазора (23 и 24) вместо
  // одного — вплотную к адресу двойной высоты она читалась слитно.
  const int16_t barY = 24, textY = 25;
  if (!s.notice) {
    drawBaudIcon(0, textY);
    display.setCursor(14, textY);
    display.print(baudBuf);
    // Текст на 102, подложка с полями занимает 100..127 — впритык к краю.
    badge(SCREEN_WIDTH - 26, textY, "TLNT", s.flags & 1, 1);
  }
#else
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("ESP Console"));
  display.setCursor(SCREEN_WIDTH - 6 * (sizeof(FW_VERSION) - 1), 0);
  display.print(F(FW_VERSION));

  display.setCursor(0, 10);
  display.print(F("AP  "));
  display.print(WiFi.softAPIP());

  display.setCursor(0, 20);
  display.print(F("STA "));
  if (s.staMode == 0)      display.print(F("off"));
  else if (s.staMode == 2) display.print(WiFi.localIP());
  else if (s.staMode == 3) display.print(F("no network"));
  else                     display.print(F("connecting"));

  display.setTextSize(2);
  display.setCursor(0, 30);
  display.print(baudBuf);
  display.setTextSize(1);

  char vBuf[8];
  dtostrf(battV, 4, 2, vBuf);
  display.setCursor(0, 48);
  display.print(F("Batt "));
  display.print(s.pct);
  display.print(F("% "));
  if (s.flags & 4) display.print(F("LOW!"));
  else { display.print(vBuf); display.print('V'); }

  const int16_t barY = 55, textY = 56;
#endif

  // Уведомление перекрывает нижнюю строку в обеих разметках.
  if (s.notice) {
    display.fillRect(0, barY, SCREEN_WIDTH, SCREEN_HEIGHT - barY, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(2, textY);
    display.print(s.notice == NOTICE_BREAK ? F("BREAK SENT") : F("CLIENT KICKED"));
    display.setTextColor(SSD1306_WHITE);
  }
#if SCREEN_HEIGHT != 32
  // На 128x32 состояние сессии показывает метка TLNT, отдельная строка не нужна.
  else if (s.flags & 1) {
    display.fillRect(0, barY, SCREEN_WIDTH, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(2, textY);
    display.print(F("TELNET: CONNECTED"));
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.setCursor(0, textY);
    display.print(F("Telnet: idle"));
  }
#endif

  display.display();
}

void setup() {
  loadSettings();

  // Буфер до begin(): при 115200 стандартных 256 байт не хватает, чтобы
  // пережить перерисовку экрана без потери вывода консоли.
  Serial.setRxBufferSize(2048);
  Serial.begin(currentSerialBaud());
  Serial.setDebugOutput(false);   // чтобы core не сорил в консольную линию

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(D2, D1);      // SDA, SCL
  Wire.setClock(400000);   // явно, не полагаясь на умолчания библиотеки

  for (uint8_t i = 0; i < sizeof(OLED_ADDRS) && !oledAddr; i++) {
    if (i2cDetect(OLED_ADDRS[i])) oledAddr = OLED_ADDRS[i];
  }
  if (oledAddr) {
    oledOk = display.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    if (oledOk) {
      applyScreenRotation();
      // Перенос строк выключаем навсегда. Он ломает и бегущую строку (текст
      // за краем экрана переносился бы посимвольно на строку ниже), и любую
      // вёрстку: длинный адрес уехал бы на следующую строку вместо обрезки.
      display.setTextWrap(false);
      display.clearDisplay();
      display.display();
      // Саму заставку рисует splashTick() на первом проходе loop() —
      // к тому моменту панель уже гарантированно вышла на режим.
      splashState = SPLASH_FILL;
      splashAt = 0;
    }
  }
#if DEBUG_SERIAL
  if (oledOk) { Serial.print(F("OLED: OK, адрес 0x")); Serial.println(oledAddr, HEX); }
  else        { Serial.println(F("OLED: НЕ НАЙДЕН")); }
#endif

  setupWifi();
  DBG("WiFi настроен");

  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("telnet", "tcp", 23);
    MDNS.addService("http", "tcp", 80);
  }

#if CAPTIVE_PORTAL
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
#endif

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  server.on("/", handleRoot);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/log", HTTP_GET, handleLog);
  server.onNotFound(handleNotFound);
  httpUpdater.setup(&server, "/update", AUTH_USER, settings.pin);
  server.begin();

  wakeScreen();
}

void loop() {
  telnetTick();
  server.handleClient();
#if CAPTIVE_PORTAL
  dnsServer.processNextRequest();
#endif
  MDNS.update();

  handleButton();
  staTick();
  sampleBattery();
  flushSettings();

  if (pendingApReconfig && millis() - pendingApAt > 300) {
    pendingApReconfig = false;
    applyApConfig();
  }

  splashTick();

  static uint32_t lastOled = 0;
  uint32_t now = millis();
  if (now - lastOled > 500) {
    lastOled = now;
    updateOled();       // внутри выходит сразу, если картинка не изменилась
    screenPowerTick();
  }
}
