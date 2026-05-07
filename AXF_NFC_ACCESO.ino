
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────────────────────────────────────────
// CONFIGURACIÓN — edita solo esta sección antes de flashear
// ─────────────────────────────────────────────────────────────────────────────

const char* WIFI_SSID     = "Mega_2.4G_6F7B";    // Nombre de tu red WiFi
const char* WIFI_PASSWORD = "7Qk93cRx";           // Contraseña WiFi
const char* SERVER_URL    = "http://192.168.1.59:3001"; // URL del backend
const char* API_KEY       = "axf_esp32_2025";     // Llave de autenticación del ESP32

// ID de la sucursal donde está instalado este dispositivo.
// Debe coincidir con el id_sucursal de la tabla `sucursales` en MySQL.
const int ID_SUCURSAL = 1;

// Tiempo mínimo entre dos lecturas NFC (milisegundos) — evita doble registro accidental
const unsigned long DEBOUNCE_MS = 2500;

// ─────────────────────────────────────────────────────────────────────────────
// PINES NFC (SPI)
// ─────────────────────────────────────────────────────────────────────────────
#define PN532_SCK   18
#define PN532_MISO  19
#define PN532_MOSI  23
#define PN532_SS     5

// ─────────────────────────────────────────────────────────────────────────────
// PINES OPCIONALES — descomenta si tienes estos componentes
// ─────────────────────────────────────────────────────────────────────────────
// #define RELAY_PIN   26   // Cerradura/torniquete
// #define LED_VERDE   27   // LED verde: acceso concedido
// #define LED_ROJO    14   // LED rojo: acceso denegado
// #define BUZZER_PIN  12   // Buzzer activo-alto

// ─────────────────────────────────────────────────────────────────────────────
// OBJETO NFC
// ─────────────────────────────────────────────────────────────────────────────
Adafruit_PN532 nfc(PN532_SS);

// ─────────────────────────────────────────────────────────────────────────────
// VARIABLE GLOBAL: timestamp de la última lectura (anti-rebote)
// ─────────────────────────────────────────────────────────────────────────────
unsigned long ultimaLectura = 0;

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────────────────────

// Convierte el UID bytes a string con formato "AA:BB:CC:DD"
String uidToString(uint8_t* uid, uint8_t len) {
  String s = "";
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
    if (i < len - 1) s += ":";
  }
  s.toUpperCase();
  return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// FEEDBACK FÍSICO (funciones seguras aunque los pines no estén definidos)
// ─────────────────────────────────────────────────────────────────────────────

void feedbackPermitido() {
  // LED verde por 2 segundos
  #ifdef LED_VERDE
    digitalWrite(LED_VERDE, HIGH);
  #endif
  // Relay abre por 3 segundos (puerta/torniquete)
  #ifdef RELAY_PIN
    digitalWrite(RELAY_PIN, HIGH);
    delay(3000);
    digitalWrite(RELAY_PIN, LOW);
  #else
    delay(2000);
  #endif
  #ifdef LED_VERDE
    digitalWrite(LED_VERDE, LOW);
  #endif
}

void feedbackDenegado() {
  // LED rojo + 3 beeps cortos
  #ifdef LED_ROJO
    digitalWrite(LED_ROJO, HIGH);
  #endif
  for (int i = 0; i < 3; i++) {
    #ifdef BUZZER_PIN
      digitalWrite(BUZZER_PIN, HIGH);
    #endif
    delay(150);
    #ifdef BUZZER_PIN
      digitalWrite(BUZZER_PIN, LOW);
    #endif
    delay(150);
  }
  delay(500);
  #ifdef LED_ROJO
    digitalWrite(LED_ROJO, LOW);
  #endif
}

// ─────────────────────────────────────────────────────────────────────────────
// ENDPOINT PRINCIPAL: POST /api/hardware/acceso/sucursal
//
// Envía el UID al backend. El backend determina si es Entrada o Salida,
// actualiza el aforo y responde con los datos del suscriptor.
//
// Body enviado:
//   { api_key, tipo:"nfc", valor:"UID", id_sucursal }
//
// Respuesta esperada:
//   { resultado, nombre, movimiento, personas_dentro }
//   resultado       → "Permitido" | "Denegado_Sin_Sub" | "Denegado_No_Encontrado"
//   movimiento      → "Entrada"   | "Salida"
//   personas_dentro → número actual de personas en la sucursal (int)
// ─────────────────────────────────────────────────────────────────────────────
void procesarAccesoSucursal(const String& uidStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Sin conexión — acceso no procesado");
    return;
  }

  Serial.println("[NFC] UID leído: " + uidStr);
  Serial.println("[HTTP] Consultando backend...");

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/hardware/acceso/sucursal");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(6000); // Timeout de 6 segundos

  // Armar body JSON
  StaticJsonDocument<256> doc;
  doc["api_key"]     = API_KEY;
  doc["tipo"]        = "nfc";
  doc["valor"]       = uidStr;
  doc["id_sucursal"] = ID_SUCURSAL;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("[HTTP] Código: %d\n", code);

  if (code != 200) {
    Serial.println("[ERROR] No se pudo contactar al servidor.");
    feedbackDenegado();
    return;
  }

  // Parsear respuesta
  StaticJsonDocument<256> respDoc;
  DeserializationError err = deserializeJson(respDoc, resp);

  if (err) {
    Serial.println("[ERROR] Respuesta JSON inválida del servidor.");
    feedbackDenegado();
    return;
  }

  const char* resultado       = respDoc["resultado"]       | "Denegado_No_Encontrado";
  const char* nombre          = respDoc["nombre"]          | "Desconocido";
  const char* movimiento      = respDoc["movimiento"]      | "-";
  int         personasDentro  = respDoc["personas_dentro"] | 0;

  // ── Mostrar resultado en Serial ─────────────────────────────────────────
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.printf("  Suscriptor : %s\n", nombre);
  Serial.printf("  Movimiento : %s\n", movimiento);
  Serial.printf("  Resultado  : %s\n", resultado);
  Serial.printf("  En sucursal: %d persona(s)\n", personasDentro);
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

  // ── Accionar feedback físico ────────────────────────────────────────────
  if (strcmp(resultado, "Permitido") == 0) {
    feedbackPermitido();
  } else {
    // Denegado_Sin_Sub o Denegado_No_Encontrado
    if (strcmp(resultado, "Denegado_Sin_Sub") == 0) {
      Serial.println("[ACCESO] Denegado — suscripción inactiva o vencida.");
    } else {
      Serial.println("[ACCESO] Denegado — tarjeta NFC no registrada en el sistema.");
    }
    feedbackDenegado();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println("\n================================================");
  Serial.println("   AXF GymNet — Control de Acceso Sucursal");
  Serial.printf ("   Sucursal ID: %d\n", ID_SUCURSAL);
  Serial.println("   Firmware v1.0 (Solo NFC)");
  Serial.println("================================================\n");

  // ── Pines de salida (opcionales) ─────────────────────────────────────────
  #ifdef RELAY_PIN
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
  #endif
  #ifdef LED_VERDE
    pinMode(LED_VERDE, OUTPUT);
    digitalWrite(LED_VERDE, LOW);
  #endif
  #ifdef LED_ROJO
    pinMode(LED_ROJO, OUTPUT);
    digitalWrite(LED_ROJO, LOW);
  #endif
  #ifdef BUZZER_PIN
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  #endif

  // ── WiFi ──────────────────────────────────────────────────────────────────
  Serial.print("[WIFI] Conectando a " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 24) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Conectado ✓  IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] No conectado — reintentando en loop");
  }

  // ── NFC ───────────────────────────────────────────────────────────────────
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    Serial.println("[NFC] ERROR — PN532 no encontrado. Verifica SCK=18, MISO=19, MOSI=23, SS=5");
  } else {
    nfc.SAMConfig();
    Serial.printf("[NFC] OK ✓  (PN5%02x firmware v%d.%d)\n",
      (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
  }

  Serial.println("\n[INFO] Listo. Esperando tarjetas NFC...\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP — Modo acceso continuo
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // ── Reconectar WiFi si se perdió ──────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Reconectando...");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // ── Anti-rebote: esperar DEBOUNCE_MS entre lecturas ───────────────────────
  if (millis() - ultimaLectura < DEBOUNCE_MS) {
    delay(100);
    return;
  }

  // ── Leer tarjeta NFC (timeout 3 segundos, no bloquea indefinidamente) ─────
  uint8_t uid[7];
  uint8_t uidLen = 0;
  bool detectado = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 3000);

  if (detectado) {
    ultimaLectura = millis(); // Registrar tiempo de lectura para anti-rebote
    String uidStr = uidToString(uid, uidLen);
    procesarAccesoSucursal(uidStr);
  }

  // Sin tarjeta detectada → volver al inicio del loop sin delay adicional
}

