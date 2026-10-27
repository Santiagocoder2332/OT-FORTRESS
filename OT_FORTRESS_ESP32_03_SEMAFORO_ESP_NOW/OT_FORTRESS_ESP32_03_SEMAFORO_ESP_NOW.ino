#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

// =====================================================
// OT-FORTRESS - ESP32-03
// Semaforo por ESP-NOW (sin Wi-Fi/MQTT)
// Usa la misma estructura de paquete que ESP32-01, ESP32-02 y ESP32-04.
// Canal ESP-NOW: 1
// =====================================================

#define CANAL_ESPNOW 1
#define MAGIC_ESPNOW 0x4F54
#define ID_MAESTRA 4

// MAC de la ESP32-04 maestra: 30:76:F5:92:04:18
const uint8_t MAC_MAESTRA[6] = {
  0x30, 0x76, 0xF5, 0x92, 0x04, 0x18
};

// Pines del semaforo
const int PIN_ROJO = 23;
const int PIN_AMARILLO = 22;
const int PIN_VERDE = 21;

// LED RGB (si es RGB de anodo comun, la logica puede ser inversa)
const int PIN_RGB_R = 12;
const int PIN_RGB_G = 13;
const int PIN_RGB_B = 14;
const int PIN_LED_NORMAL = 2;

// Tiempos
const unsigned long TIEMPO_VERDE_MS = 10000;
const unsigned long TIEMPO_HASTA_ROJO_MS = 20000;
const unsigned long TIMEOUT_MAESTRA_MS = 5000;
const unsigned long INTERVALO_LATIDO_MS = 1000;

// Tipos de mensaje compartidos
enum TipoMensajeESPNow : uint8_t {
  TIPO_CONTEO = 1,
  TIPO_ALTURA = 2,
  TIPO_ALARMA = 3,
  TIPO_ESTADO = 4,
  TIPO_CLASIFICACION = 5,
  TIPO_LATIDO = 6,
  TIPO_COMANDO = 100
};

struct __attribute__((packed)) PaqueteESPNow {
  uint16_t magic;
  uint8_t nodo;
  uint8_t tipo;
  uint16_t secuencia;
  int32_t valor1;
  int32_t valor2;
  uint32_t flags;
  char texto[32];
};

QueueHandle_t colaESPNow = nullptr;

volatile unsigned long ultimoMensajeMaestra = 0;
uint32_t ultimoConteo = 0;
bool conteoInicializado = false;
bool cicloActivo = false;
bool alarmaActiva = false;
unsigned long inicioCiclo = 0;
unsigned long ultimoLatidoPublicado = 0;
uint16_t secuenciaEstado = 0;

bool maestroConectado = false;
String ultimoEstado = "";

// Callback radio: solo valida y pone el paquete en la cola.
void recibirESPNow(const esp_now_recv_info_t *info,
                   const uint8_t *datos, int longitud) {
  if (!info || !datos) return;
  if (memcmp(info->src_addr, MAC_MAESTRA, 6) != 0) return;
  if (longitud != sizeof(PaqueteESPNow)) return;

  PaqueteESPNow p;
  memcpy(&p, datos, sizeof(p));
  if (p.magic != MAGIC_ESPNOW) return;
  if (colaESPNow) xQueueSend(colaESPNow, &p, 0);
}

void apagarSemaforo() {
  digitalWrite(PIN_ROJO, LOW);
  digitalWrite(PIN_AMARILLO, LOW);
  digitalWrite(PIN_VERDE, LOW);
}

void encenderLuz(int pin) {
  apagarSemaforo();
  digitalWrite(pin, HIGH);
}

bool maestroVivo() {
  return (millis() - ultimoMensajeMaestra) <= TIMEOUT_MAESTRA_MS;
}

const char* calcularEstado() {
  if (!maestroVivo() || alarmaActiva || !cicloActivo) return "rojo";

  unsigned long transcurrido = millis() - inicioCiclo;
  if (transcurrido < TIEMPO_VERDE_MS) return "verde";
  if (transcurrido < TIEMPO_HASTA_ROJO_MS) return "amarillo";
  return "rojo";
}

void controlarRGB() {
  if (!maestroVivo() || alarmaActiva) {
    unsigned long fase = (millis() / 700) % 3;
    digitalWrite(PIN_RGB_R, fase == 0 ? HIGH : LOW);
    digitalWrite(PIN_RGB_G, fase == 1 ? HIGH : LOW);
    digitalWrite(PIN_RGB_B, fase == 2 ? HIGH : LOW);
  } else {
    digitalWrite(PIN_RGB_R, HIGH);
    digitalWrite(PIN_RGB_G, HIGH);
    digitalWrite(PIN_RGB_B, HIGH);
  }
}

void publicarEstadoLocal(const char* estado) {
  if (ultimoEstado == estado) return;
  ultimoEstado = estado;
  Serial.print("[SEMAFORO] Estado: ");
  Serial.println(estado);
}

void actualizarSemaforo() {
  const char* estado = calcularEstado();

  if (strcmp(estado, "verde") == 0) encenderLuz(PIN_VERDE);
  else if (strcmp(estado, "amarillo") == 0) encenderLuz(PIN_AMARILLO);
  else encenderLuz(PIN_ROJO);

  publicarEstadoLocal(estado);
}

void procesarPaquete(const PaqueteESPNow &p) {
  ultimoMensajeMaestra = millis();

  switch (p.tipo) {
    case TIPO_LATIDO:
      // La maestra envia un latido cada segundo.
      break;

    case TIPO_CONTEO: {
      if (p.valor1 < 0) break;
      uint32_t nuevoConteo = (uint32_t)p.valor1;

      if (!conteoInicializado) {
        ultimoConteo = nuevoConteo;
        conteoInicializado = true;
        Serial.printf("[CONTEO] Sincronizado: %lu\n",
                      (unsigned long)ultimoConteo);
        break;
      }

      if (nuevoConteo <= ultimoConteo) break;
      ultimoConteo = nuevoConteo;

      Serial.printf("[CONTEO] Nuevo producto: %lu\n",
                    (unsigned long)ultimoConteo);

      if (!alarmaActiva) {
        inicioCiclo = millis();
        cicloActivo = true;
      }
      break;
    }

    case TIPO_ALTURA:
      Serial.printf("[ALTURA] Producto #%ld, altura %.2f cm\n",
                    (long)p.valor2, (float)p.valor1 / 100.0f);
      break;

    case TIPO_ALARMA:
      if (p.valor1 != 0 || strncmp(p.texto, "ALARMA", 6) == 0) {
        alarmaActiva = true;
        cicloActivo = false;
        Serial.printf("[ALARMA] Activa: %s\n", p.texto);
      } else {
        alarmaActiva = false;
        cicloActivo = false;
        Serial.println("[ALARMA] Sistema normal; esperando producto.");
      }
      break;

    case TIPO_CLASIFICACION:
      Serial.printf("[CLASIFICACION] %s\n", p.texto);
      if (!alarmaActiva) {
        inicioCiclo = millis();
        cicloActivo = true;
      }
      break;

    default:
      Serial.printf("[ESP-NOW] Tipo no utilizado: %u\n", p.tipo);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_ROJO, OUTPUT);
  pinMode(PIN_AMARILLO, OUTPUT);
  pinMode(PIN_VERDE, OUTPUT);
  pinMode(PIN_RGB_R, OUTPUT);
  pinMode(PIN_RGB_G, OUTPUT);
  pinMode(PIN_RGB_B, OUTPUT);
  pinMode(PIN_LED_NORMAL, OUTPUT);

  digitalWrite(PIN_LED_NORMAL, HIGH);
  apagarSemaforo();
  encenderLuz(PIN_ROJO);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();

  Serial.println();
  Serial.println("==================================");
  Serial.println("OT-FORTRESS - ESP32-03 SEMAFORO");
  Serial.println("Comunicacion ESP-NOW con ESP32-04");
  Serial.println("Verde: 10 s; amarillo: hasta 20 s");
  Serial.println("Rojo: espera, alarma o perdida de enlace");
  Serial.println("==================================");
  Serial.printf("MAC propia: %s\n", WiFi.macAddress().c_str());

  esp_err_t canal = esp_wifi_set_channel(
    CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE
  );
  if (canal != ESP_OK) {
    Serial.printf("[ERROR] Canal Wi-Fi: %d\n", (int)canal);
    while (true) delay(1000);
  }

  colaESPNow = xQueueCreate(20, sizeof(PaqueteESPNow));
  if (!colaESPNow) {
    Serial.println("[ERROR] No se pudo crear la cola.");
    while (true) delay(1000);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] No se pudo iniciar ESP-NOW.");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(recibirESPNow);

  // Añadir la maestra como peer para enviar estado de vuelta.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_MAESTRA, 6);
  peer.channel = CANAL_ESPNOW;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(MAC_MAESTRA)) {
    esp_err_t r = esp_now_add_peer(&peer);
    if (r != ESP_OK) {
      Serial.printf("[ERROR] Añadiendo maestra: %d\n", (int)r);
    }
  }

  ultimoMensajeMaestra = millis();
  maestroConectado = true;
  Serial.println("[OK] ESP-NOW iniciado; esperando latidos de la maestra.");
}

void loop() {
  PaqueteESPNow p;
  while (colaESPNow && xQueueReceive(colaESPNow, &p, 0) == pdTRUE) {
    procesarPaquete(p);
  }

  bool vivoAhora = maestroVivo();
  if (vivoAhora != maestroConectado) {
    maestroConectado = vivoAhora;
    Serial.println(maestroConectado
      ? "[ENLACE] Maestra conectada."
      : "[ALARMA] Se perdio comunicacion con la maestra.");
  }

  actualizarSemaforo();
  controlarRGB();

  // Envia estado al maestro cada segundo. La maestra debe aceptar
  // paquetes del nodo 3 si se desea registrar este estado centralmente.
  if (millis() - ultimoLatidoPublicado >= INTERVALO_LATIDO_MS) {
    ultimoLatidoPublicado = millis();

    PaqueteESPNow estado = {};
    estado.magic = MAGIC_ESPNOW;
    estado.nodo = 3;
    estado.tipo = TIPO_ESTADO;
    estado.secuencia = ++secuenciaEstado;
    estado.valor1 = maestroVivo() ? 1 : 0;
    estado.valor2 = alarmaActiva ? 1 : 0;
    strncpy(estado.texto, ultimoEstado.c_str(), sizeof(estado.texto) - 1);

    esp_now_send(MAC_MAESTRA,
                 (const uint8_t *)&estado, sizeof(estado));
  }

  delay(10);
}
