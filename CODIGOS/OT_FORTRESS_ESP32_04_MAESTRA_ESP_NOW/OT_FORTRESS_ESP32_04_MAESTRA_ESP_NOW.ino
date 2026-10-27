
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#define CANAL_ESPNOW 1
#define MAGIC_ESPNOW 0x4F54

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

const uint8_t MAC_DIFUSION[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

QueueHandle_t colaRecepcion = nullptr;
uint16_t secuenciaLatido = 0;
unsigned long ultimoLatido = 0;

void alRecibir(const esp_now_recv_info_t *info,
               const uint8_t *datos, int longitud) {
  if (info == nullptr || datos == nullptr) return;
  if (longitud != sizeof(PaqueteESPNow)) return;

  PaqueteESPNow p;
  memcpy(&p, datos, sizeof(p));

  if (p.magic != MAGIC_ESPNOW) return;
  if (p.nodo != 2 && p.nodo != 3) return;

  if (colaRecepcion != nullptr) {
    xQueueSend(colaRecepcion, &p, 0);
  }
}

bool difundir(const PaqueteESPNow &p) {
  esp_err_t r = esp_now_send(
    MAC_DIFUSION,
    reinterpret_cast<const uint8_t *>(&p),
    sizeof(p)
  );

  if (r != ESP_OK) {
    Serial.printf("[ERROR] Difusion ESP-NOW: %d\n", (int)r);
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();
  delay(200);

  Serial.println();
  Serial.println("======================================");
  Serial.println("OT-FORTRESS ESP32-04 MAESTRA");
  Serial.println("======================================");

  // Leer la MAC directamente desde la interfaz STA.
  uint8_t mac[6] = {0};
  esp_err_t resultadoMac = esp_wifi_get_mac(WIFI_IF_STA, mac);

  if (resultadoMac == ESP_OK) {
    Serial.printf(
      "MAC maestra: %02X:%02X:%02X:%02X:%02X:%02X\n",
      mac[0], mac[1], mac[2],
      mac[3], mac[4], mac[5]
    );
  } else {
    Serial.printf("[ERROR] Lectura de MAC: %d\n",
                  (int)resultadoMac);
  }

  esp_err_t canal = esp_wifi_set_channel(
    CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE
  );

  if (canal != ESP_OK) {
    Serial.printf("[ERROR] Canal: %d\n", (int)canal);
    while (true) delay(1000);
  }

  colaRecepcion = xQueueCreate(30, sizeof(PaqueteESPNow));

  if (colaRecepcion == nullptr) {
    Serial.println("[ERROR] No se pudo crear la cola.");
    while (true) delay(1000);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] No se pudo iniciar ESP-NOW.");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(alRecibir);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_DIFUSION, 6);
  peer.channel = CANAL_ESPNOW;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(MAC_DIFUSION)) {
    esp_err_t r = esp_now_add_peer(&peer);

    if (r != ESP_OK) {
      Serial.printf("[ERROR] Añadiendo difusion: %d\n",
                    (int)r);
      while (true) delay(1000);
    }
  }

  Serial.println("[OK] ESP-NOW iniciado.");
  Serial.println("[OK] Retransmitiendo conteo y altura.");
  Serial.println("[OK] Latido al semaforo cada 1 segundo.");
}

void loop() {
  // Latido para mantener informado al semaforo.
  if (millis() - ultimoLatido >= 1000) {
    ultimoLatido = millis();

    PaqueteESPNow p = {};
    p.magic = MAGIC_ESPNOW;
    p.nodo = 4;
    p.tipo = TIPO_LATIDO;
    p.secuencia = ++secuenciaLatido;

    strncpy(p.texto, "LATIDO_MAESTRA",
            sizeof(p.texto) - 1);

    difundir(p);
  }

  PaqueteESPNow p;

  if (colaRecepcion != nullptr &&
      xQueueReceive(colaRecepcion, &p,
                    pdMS_TO_TICKS(20)) == pdTRUE) {

    Serial.printf(
      "\n[RECIBIDO] Nodo %u, tipo %u, secuencia %u\n",
      p.nodo, p.tipo, p.secuencia
    );

    Serial.printf(
      "Valor1=%ld Valor2=%ld Texto=%s\n",
      (long)p.valor1, (long)p.valor2, p.texto
    );

    // Retransmitir conteo, altura y alarmas del nodo 2.
    if (p.nodo == 2 &&
        (p.tipo == TIPO_CONTEO ||
         p.tipo == TIPO_ALTURA ||
         p.tipo == TIPO_ALARMA)) {

      if (p.tipo == TIPO_ALTURA) {
        Serial.printf(
          "[ALTURA] Producto #%ld: %.2f cm\n",
          (long)p.valor2,
          (float)p.valor1 / 100.0f
        );
      }

      if (p.tipo == TIPO_CONTEO) {
        Serial.printf("[CONTEO] %ld\n", (long)p.valor1);
      }

      if (difundir(p)) {
        Serial.println("[REENVIADO] Paquete del nodo 2.");
      }
    }

    // Mostrar el estado del semaforo.
    else if (p.nodo == 3 && p.tipo == TIPO_ESTADO) {
      Serial.printf(
        "[SEMAFORO] Estado=%s, enlace=%ld, alarma=%ld\n",
        p.texto, (long)p.valor1, (long)p.valor2
      );
    }
  }

  delay(1);
}
