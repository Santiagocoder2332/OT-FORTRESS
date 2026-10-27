
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// =====================================================
// OT-FORTRESS - ESP32-02
// Conteo por infrarrojo y altura por ultrasonido
// =====================================================

// ---------------- ESP-NOW ----------------------------

// Sustituir por la MAC real de la ESP32-04 maestra.

uint8_t MAC_MAESTRA[6] = {
  0x30, 0x76, 0xF5, 0x92, 0x04, 0x18
};

#define CANAL_ESPNOW 1
#define MAGIC_ESPNOW 0x4F54

enum TipoPaqueteESPNow : uint8_t {
  TIPO_CONTEO = 1,
  TIPO_ALTURA = 2,
  TIPO_ALARMA = 3,
  TIPO_ESTADO = 4,
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

uint16_t secuenciaESPNow = 0;
bool conteoPendiente = true;

Preferences preferencias;

// ---------------- PINES ------------------------------

const int PIN_TRIG = 15;
const int PIN_ECHO = 16;
const int PIN_HW201 = 22;

// Cambiar a HIGH si el HW-201 activa su salida en HIGH.
const int SENSOR_ACTIVO = LOW;

// ---------------- CALIBRACION ------------------------

const float DISTANCIA_BANDA_VACIA_CM = 10.99;
const float ALTURA_MINIMA_CM = 0.50;

const unsigned long ANTIRREBOTE_MS = 35;
const unsigned long INTERVALO_ULTRASONICO_MS = 60;
const unsigned long TIMEOUT_ECHO_US = 15000;
const unsigned long TIEMPO_MAX_ESPERA_MS = 20000;

const uint8_t LECTURAS_OBJETO_NECESARIAS = 2;
const uint8_t LECTURAS_BANDA_NECESARIAS = 3;
const uint8_t LECTURAS_SIN_ECO_NECESARIAS = 5;

// ---------------- RECONEXION -------------------------

const unsigned long INTERVALO_WIFI_MS = 10000;
const unsigned long INTERVALO_MQTT_MS = 5000;

unsigned long ultimoIntentoWiFi = 0;
unsigned long ultimoIntentoMQTT = 0;

// =====================================================
// EVENTOS ENTRE LA TAREA DE SENSORES Y MQTT
// =====================================================

enum TipoEvento : uint8_t {
  EVENTO_CONTEO,
  EVENTO_ALTURA,
  EVENTO_ERROR_MEDICION
};

struct EventoSensor {
  TipoEvento tipo;
  uint32_t numeroProducto;
  float altura;
};

struct RegistroAltura {
  uint32_t numeroProducto;
  float altura;
};

QueueHandle_t colaEventos = nullptr;

// Conteo inicial recuperado de la memoria no volatil.
uint32_t conteoInicial = 0;

// Variables gestionadas por loop().
uint32_t conteoActual = 0;

// =====================================================
// ENVIAR EVENTOS A LA TAREA PRINCIPAL
// =====================================================

void emitirEvento(
  TipoEvento tipo,
  uint32_t numeroProducto = 0,
  float altura = 0.0
) {
  if (colaEventos == nullptr) {
    return;
  }

  EventoSensor evento = {};

  evento.tipo = tipo;
  evento.numeroProducto = numeroProducto;
  evento.altura = altura;

  if (xQueueSend(
        colaEventos,
        &evento,
        pdMS_TO_TICKS(20)
      ) != pdTRUE) {

    Serial.println("ERROR: cola de eventos llena.");
  }
}

// =====================================================
// LEER EL HC-SR04
// Esta distancia es interna: no se publica por MQTT.
// =====================================================

bool leerDistancia(float &distancia) {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);

  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);

  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracion = pulseIn(
    PIN_ECHO,
    HIGH,
    TIMEOUT_ECHO_US
  );

  if (duracion == 0) {
    return false;
  }

  distancia = duracion * 0.0343f / 2.0f;

  if (distancia < 2.0 || distancia > 400.0) {
    return false;
  }

  return true;
}

// =====================================================
// TERMINAR LA MEDICION DE UN PRODUCTO
// Solo se genera un dato MQTT: la altura.
// =====================================================

void completarMedicion(
  uint32_t numeroProducto,
  float distanciaMinima
) {
  float altura =
    DISTANCIA_BANDA_VACIA_CM - distanciaMinima;

  if (altura < ALTURA_MINIMA_CM) {
    emitirEvento(
      EVENTO_ERROR_MEDICION,
      numeroProducto
    );

    return;
  }

  // La distancia no se publica ni se guarda en la cola.
  emitirEvento(
    EVENTO_ALTURA,
    numeroProducto,
    altura
  );

  Serial.print("Producto: ");
  Serial.println((unsigned long)numeroProducto);

  Serial.print("Altura: ");
  Serial.print(altura, 2);
  Serial.println(" cm");
}

// =====================================================
// TAREA INDEPENDIENTE DE LOS SENSORES
//
// 1. Cuenta cada producto detectado por el HW-201.
// 2. Deja que el producto avance por la banda.
// 3. Espera a detectar ese producto con el HC-SR04.
// 4. Calcula la altura y entrega solo ese valor.
// =====================================================

void tareaSensores(void* parametro) {
  const uint8_t MAX_PENDIENTES = 32;

  uint32_t pendientes[MAX_PENDIENTES];
  uint8_t cabeza = 0;
  uint8_t finalCola = 0;
  uint8_t cantidadPendientes = 0;

  uint32_t contador = conteoInicial;

  bool lecturaCandidata =
    (digitalRead(PIN_HW201) == SENSOR_ACTIVO);

  bool estadoIR = false;
  unsigned long inicioCambioIR = millis();

  bool esperandoObjeto = false;
  bool midiendoObjeto = false;

  unsigned long inicioEspera = 0;
  unsigned long ultimaLecturaUltrasonica = 0;

  uint32_t productoActual = 0;

  uint8_t lecturasObjeto = 0;
  uint8_t lecturasBanda = 0;
  uint8_t lecturasSinEco = 0;

  float distanciaCandidataMinima = 0.0;
  float distanciaMinima = 0.0;

  const float LIMITE_OBJETO =
    DISTANCIA_BANDA_VACIA_CM - ALTURA_MINIMA_CM;

  for (;;) {
    unsigned long ahora = millis();

    // -----------------------------------------------
    // 1. INFRARROJO: DETECTAR Y CONTAR PRODUCTOS
    // -----------------------------------------------

    bool lecturaActual =
      (digitalRead(PIN_HW201) == SENSOR_ACTIVO);

    if (lecturaActual != lecturaCandidata) {
      lecturaCandidata = lecturaActual;
      inicioCambioIR = ahora;
    }

    if (ahora - inicioCambioIR >= ANTIRREBOTE_MS &&
        lecturaCandidata != estadoIR) {

      estadoIR = lecturaCandidata;

      // Contar solamente al entrar un producto.
      if (estadoIR) {
        if (contador < 0xFFFFFFFFUL) {
          contador++;
        }

        emitirEvento(
          EVENTO_CONTEO,
          contador
        );

        Serial.print("Conteo IR: ");
        Serial.println((unsigned long)contador);

        if (cantidadPendientes < MAX_PENDIENTES) {
          pendientes[finalCola] = contador;

          finalCola =
            (finalCola + 1) % MAX_PENDIENTES;

          cantidadPendientes++;

        } else {
          Serial.println(
            "ERROR: cola de productos llena; conteo conservado."
          );

          emitirEvento(
            EVENTO_ERROR_MEDICION,
            contador
          );
        }
      }
    }

    // -----------------------------------------------
    // 2. ESPERAR AL PRODUCTO EN EL ULTRASONICO
    // -----------------------------------------------

    if (!midiendoObjeto &&
        !esperandoObjeto &&
        cantidadPendientes > 0) {

      esperandoObjeto = true;
      inicioEspera = ahora;
      lecturasObjeto = 0;
      distanciaCandidataMinima = 0.0;
    }

    // Si no hay productos pendientes, no medir la banda.
    if (!esperandoObjeto && !midiendoObjeto) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Respetar el intervalo entre pulsos ultrasónicos.
    if (ahora - ultimaLecturaUltrasonica <
        INTERVALO_ULTRASONICO_MS) {

      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    ultimaLecturaUltrasonica = ahora;

    float distancia = 0.0;
    bool ecoValido = leerDistancia(distancia);

    // -----------------------------------------------
    // 3. ESPERAR A QUE LLEGUE EL PRODUCTO
    // -----------------------------------------------

    if (esperandoObjeto) {

      if (ahora - inicioEspera >=
          TIEMPO_MAX_ESPERA_MS) {

        uint32_t numeroFallido = pendientes[cabeza];

        cabeza =
          (cabeza + 1) % MAX_PENDIENTES;

        cantidadPendientes--;

        esperandoObjeto = false;

        emitirEvento(
          EVENTO_ERROR_MEDICION,
          numeroFallido
        );

        continue;
      }

      if (!ecoValido) {
        lecturasObjeto = 0;
        continue;
      }

      // Si sigue viendo la banda vacía, no se registra altura.
      if (distancia < LIMITE_OBJETO) {

        if (lecturasObjeto == 0) {
          distanciaCandidataMinima = distancia;

        } else if (
          distancia < distanciaCandidataMinima
        ) {
          distanciaCandidataMinima = distancia;
        }

        lecturasObjeto++;

      } else {
        lecturasObjeto = 0;
      }

      // Confirmar el objeto con dos lecturas cercanas.
      if (lecturasObjeto >= LECTURAS_OBJETO_NECESARIAS) {

        productoActual = pendientes[cabeza];

        cabeza =
          (cabeza + 1) % MAX_PENDIENTES;

        cantidadPendientes--;

        esperandoObjeto = false;
        midiendoObjeto = true;

        distanciaMinima = distanciaCandidataMinima;

        lecturasBanda = 0;
        lecturasSinEco = 0;

        Serial.print("Ultrasonico detecto producto #");
        Serial.println((unsigned long)productoActual);
      }

      continue;
    }

    // -----------------------------------------------
    // 4. MEDIR EL PRODUCTO EN MOVIMIENTO
    // -----------------------------------------------

    if (midiendoObjeto) {

      if (ecoValido) {
        lecturasSinEco = 0;

        if (distancia < LIMITE_OBJETO) {

          if (distancia < distanciaMinima) {
            distanciaMinima = distancia;
          }

          lecturasBanda = 0;

        } else {
          // La distancia volvió a corresponder a la banda.
          lecturasBanda++;

          if (lecturasBanda >= LECTURAS_BANDA_NECESARIAS) {

            completarMedicion(
              productoActual,
              distanciaMinima
            );

            midiendoObjeto = false;
            lecturasBanda = 0;
            lecturasSinEco = 0;
          }
        }

      } else {
        lecturasSinEco++;

        if (lecturasSinEco >= LECTURAS_SIN_ECO_NECESARIAS) {

          // Se usa la menor distancia válida observada.
          completarMedicion(
            productoActual,
            distanciaMinima
          );

          midiendoObjeto = false;
          lecturasBanda = 0;
          lecturasSinEco = 0;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// =====================================================
// ENVIO DE PAQUETES ESP-NOW A LA MAESTRA
// =====================================================

bool enviarESPNow(uint8_t tipo, int32_t valor1,
                  int32_t valor2, const char* texto) {
  PaqueteESPNow p = {};
  p.magic = MAGIC_ESPNOW;
  p.nodo = 2;
  p.tipo = tipo;
  p.secuencia = ++secuenciaESPNow;
  p.valor1 = valor1;
  p.valor2 = valor2;
  p.flags = 0;

  if (texto != nullptr) {
    strncpy(p.texto, texto, sizeof(p.texto) - 1);
  }

  esp_err_t resultado = esp_now_send(
    MAC_MAESTRA,
    reinterpret_cast<const uint8_t*>(&p),
    sizeof(p)
  );

  if (resultado != ESP_OK) {
    Serial.printf("[ESP-NOW] Error de envio: %d\\n", (int)resultado);
    return false;
  }
  return true;
}

// =====================================================
// PROCESAR EVENTOS Y ENVIARLOS A LA MAESTRA
// =====================================================

void procesarEventosSensores() {
  EventoSensor evento;

  while (xQueueReceive(colaEventos, &evento, 0) == pdTRUE) {
    if (evento.tipo == EVENTO_CONTEO) {
      conteoActual = evento.numeroProducto;

      // Persistir el conteo para recuperarlo tras un reinicio.
      preferencias.putUInt("conteo", conteoActual);
      conteoPendiente = true;

      Serial.printf("[ESP-NOW] Conteo pendiente: %lu\\n",
                    (unsigned long)conteoActual);
    }
    else if (evento.tipo == EVENTO_ALTURA) {
      // Transmitir altura en centésimas de cm: 425 = 4.25 cm.
      int32_t alturaCentecm = (int32_t)lroundf(evento.altura * 100.0f);

      if (enviarESPNow(TIPO_ALTURA, alturaCentecm,
                       (int32_t)evento.numeroProducto,
                       "ALTURA_CM_X100")) {
        Serial.printf("[ESP-NOW] Altura enviada: producto=%lu, altura=%.2f cm\\n",
                      (unsigned long)evento.numeroProducto,
                      evento.altura);
      }
    }
    else if (evento.tipo == EVENTO_ERROR_MEDICION) {
      Serial.printf("[ERROR] Sin altura valida para producto #%lu\\n",
                    (unsigned long)evento.numeroProducto);
      enviarESPNow(TIPO_ALARMA, (int32_t)evento.numeroProducto,
                   1, "ERROR_MEDICION");
    }
  }
}

// =====================================================
// ENVIAR EL CONTEO ACTUAL A LA MAESTRA
// Incluye el conteo recuperado de Preferences al arrancar.
// =====================================================

void publicarConteo() {
  if (!conteoPendiente) return;

  if (enviarESPNow(TIPO_CONTEO, (int32_t)conteoActual,
                   0, "CONTEO_IR")) {
    conteoPendiente = false;
    Serial.printf("[ESP-NOW] Conteo enviado: %lu\\n",
                  (unsigned long)conteoActual);
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);

  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_HW201, INPUT);

  // Recuperar el conteo previo.
  preferencias.begin("ot-esp2", false);

  conteoInicial = preferencias.getUInt("conteo", 0);
  conteoActual = conteoInicial;

  // Cola de eventos entre la tarea de sensores y loop().
  colaEventos = xQueueCreate(64, sizeof(EventoSensor));

  if (colaEventos == nullptr) {
    Serial.println("ERROR: no se pudo crear la cola de eventos.");
    while (true) delay(1000);
  }

  // Inicializar ESP-NOW en modo estación, sin conectarse a un router.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();

  esp_err_t canalOk = esp_wifi_set_channel(
    CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE
  );

  if (canalOk != ESP_OK) {
    Serial.println("[ERROR] No se pudo configurar el canal ESP-NOW.");
    while (true) delay(1000);
  }

  Serial.print("[MAC ESP32-02] ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] No se pudo iniciar ESP-NOW.");
    while (true) delay(1000);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_MAESTRA, 6);
  peer.channel = CANAL_ESPNOW;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(MAC_MAESTRA)) {
    if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("[ERROR] No se pudo agregar la maestra como peer.");
      while (true) delay(1000);
    }
  }

  Serial.println();
  Serial.println("==================================");
  Serial.println("OT-FORTRESS - ESP32-02");
  Serial.print("Conteo recuperado: ");
  Serial.println((unsigned long)conteoActual);
  Serial.println("Comunicacion: ESP-NOW hacia ESP32-04");
  Serial.println(" - Conteo IR");
  Serial.println(" - Altura en centesimas de cm");
  Serial.println(" - Alarmas de medicion");
  Serial.println("==================================");

  BaseType_t resultado = xTaskCreatePinnedToCore(
    tareaSensores,
    "TareaSensores",
    4096,
    nullptr,
    2,
    nullptr,
    1
  );

  if (resultado != pdPASS) {
    Serial.println("ERROR: no se pudo iniciar la tarea de sensores.");
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  procesarEventosSensores();
  publicarConteo();

  delay(1);
}
