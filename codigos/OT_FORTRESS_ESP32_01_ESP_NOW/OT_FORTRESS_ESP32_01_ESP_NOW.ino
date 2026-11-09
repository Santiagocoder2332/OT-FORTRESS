#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESP32Servo.h
#include <math.h> 
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// =====================================================
// OT-FORTRESS - ESP32-01
// Motor + servo + final de carrera
// Comunicacion ESP-NOW con ESP32-04 (maestra)
// =====================================================

// IMPORTANTE:
// 1) Sustituye MAC_MAESTRA por la MAC real de ESP32-04.
// 2) La maestra debe reenviar los mensajes de ESP32-02 a este nodo.
// 3) Todos los dispositivos deben usar el mismo canal ESP-NOW.
// 4) Este nodo recibe conteo/altura; no usa MQTT ni se conecta al hotspot.

#define CANAL_ESPNOW 1
#define MAGIC_ESPNOW 0x4F54
#define ID_NODO 1

// Reemplaza estos bytes con la MAC REAL de ESP32-04.
// Ejemplo de formato: AA:BB:CC:11:22:33 -> 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33

uint8_t MAC_MAESTRA[6] = {
  0x30, 0x76, 0xF5, 0x92, 0x04, 0x18
};


enum TipoMensajeESPNow : uint8_t {
  TIPO_CONTEO = 1,   // valor1 = conteo acumulado
  TIPO_ALTURA = 2,   // valor1 = altura en centesimas de cm (425 = 4.25 cm)
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

QueueHandle_t colaESPNow = nullptr;

// PINES
#define IN1 26
#define IN2 33
#define IN3 25
#define IN4 32
#define SERVO_PIN 13
#define FINAL_CARRERA_PIN 27

// MOTOR 28BYJ-48 + ULN2003
const int pinesMotor[4] = {IN1, IN2, IN3, IN4};

const int secuencia[8][4] = {
  {1, 0, 0, 0},
  {1, 1, 0, 0},
  {0, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 0},
  {0, 0, 1, 1},
  {0, 0, 0, 1},
  {1, 0, 0, 1}
};

const unsigned long INTERVALO_PASO_US = 3000;
int posicionMotor = 0;
volatile bool bandaActiva = false;

// SERVO
const int ANGULO_REPOSO = 180;
const int ANGULO_PEQUENO = 140;
const int ANGULO_GRANDE = 90;
const unsigned long TIEMPO_SERVO_GRANDE_MS = 13000;
const unsigned long TIEMPO_BANDA_MS = 25000;
Servo compuerta;

enum ModoServo {
  SERVO_REPOSO,
  SERVO_PEQUENO_ESPERA_FINAL,
  SERVO_GRANDE_ABIERTO
};

ModoServo modoServo = SERVO_REPOSO;
unsigned long inicioServoGrande = 0;

// Temporizador independiente de parada de la banda
bool temporizadorParadaActivo = false;
unsigned long inicioTemporizadorParada = 0;

// FINAL DE CARRERA
int lecturaAnteriorFinal = HIGH;
int estadoEstableFinal = HIGH;
bool finalPresionadoAnterior = false;
unsigned long ultimoCambioFinal = 0;
const unsigned long ANTIRREBOTE_FINAL_MS = 40;
bool bloqueoPorFinalCarrera = false;

// CONTEO RECIBIDO POR ESP-NOW
uint32_t ultimoConteoRecibido = 0;
bool conteoInicializado = false;

// COLA DE PRODUCTOS PENDIENTES DE ALTURA
const uint8_t MAX_PRODUCTOS = 32;
uint32_t colaIDs[MAX_PRODUCTOS];
uint8_t cabezaIDs = 0;
uint8_t finalIDs = 0;
uint8_t cantidadIDs = 0;

// TRABAJOS DE CLASIFICACION
const uint8_t MAX_TRABAJOS = 24;

enum TipoTrabajo {
  TRABAJO_PEQUENO,
  TRABAJO_GRANDE
};

struct Trabajo {
  bool activo;
  TipoTrabajo tipo;
  uint32_t numeroProducto;
};

Trabajo trabajos[MAX_TRABAJOS];

// =====================================================
// MOTOR: APAGAR BOBINAS
// =====================================================
void apagarMotor() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(pinesMotor[i], LOW);
  }
}

// =====================================================
// MOTOR: GIRO INVERSO
// =====================================================
void avanzarMotorInverso() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(pinesMotor[i], secuencia[posicionMotor][i]);
  }

  posicionMotor--;
  if (posicionMotor < 0) posicionMotor = 7;
}

// =====================================================
// TAREA INDEPENDIENTE DEL MOTOR
// =====================================================
void tareaMotor(void* parametro) {
  unsigned long ultimoPaso = micros();
  bool motorEstabaActivo = false;

  for (;;) {
    bool activa = bandaActiva;

    if (activa) {
      if (!motorEstabaActivo) {
        motorEstabaActivo = true;
        ultimoPaso = micros();
      }

      unsigned long ahoraUs = micros();
      if (ahoraUs - ultimoPaso >= INTERVALO_PASO_US) {
        ultimoPaso = ahoraUs;
        avanzarMotorInverso();
      }
    } else if (motorEstabaActivo) {
      apagarMotor();
      motorEstabaActivo = false;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// =====================================================
// COLA DE IDS
// =====================================================
bool encolarID(uint32_t id) {
  if (cantidadIDs >= MAX_PRODUCTOS) {
    Serial.println("[ERROR] Cola de productos llena.");
    return false;
  }

  colaIDs[finalIDs] = id;
  finalIDs = (finalIDs + 1) % MAX_PRODUCTOS;
  cantidadIDs++;
  return true;
}

uint32_t sacarID() {
  if (cantidadIDs == 0) return 0;

  uint32_t id = colaIDs[cabezaIDs];
  cabezaIDs = (cabezaIDs + 1) % MAX_PRODUCTOS;
  cantidadIDs--;
  return id;
}

// =====================================================
// CONTEO IR RECIBIDO DESDE LA MAESTRA
// =====================================================
void registrarNuevoConteo(uint32_t nuevoConteo) {
  if (nuevoConteo <= ultimoConteoRecibido) return;

  while (ultimoConteoRecibido < nuevoConteo) {
    uint32_t id = ultimoConteoRecibido + 1;

    if (!encolarID(id)) {
      ultimoConteoRecibido = id;
      continue;
    }

    ultimoConteoRecibido = id;
    Serial.print("[IR/ESP-NOW] Objeto contado: ");
    Serial.println((unsigned long)id);

    bloqueoPorFinalCarrera = false;
    bandaActiva = true;

    if (temporizadorParadaActivo) {
      inicioTemporizadorParada = millis();
      Serial.println("[IR] Nuevo objeto: reiniciando temporizador de parada.");
    }

    Serial.println("[IR] Banda en marcha. Servo sin cambios.");
  }
}

// =====================================================
// PROGRAMAR ACTUACION SEGUN ALTURA
// =====================================================
bool programarTrabajo(uint32_t numeroProducto, float altura) {
  int libre = -1;

  for (int i = 0; i < MAX_TRABAJOS; i++) {
    if (!trabajos[i].activo) {
      libre = i;
      break;
    }
  }

  if (libre < 0) {
    Serial.println("[ERROR] Cola de actuaciones llena.");
    return false;
  }

  Trabajo &t = trabajos[libre];
  t.activo = true;
  t.numeroProducto = numeroProducto;

  if (altura < 5.0) {
    t.tipo = TRABAJO_PEQUENO;
    Serial.print("[HC-SR04] Producto #");
    Serial.print((unsigned long)numeroProducto);
    Serial.println(" menor de 5 cm: apertura a 140 grados.");
  } else {
    t.tipo = TRABAJO_GRANDE;
    Serial.print("[HC-SR04] Producto #");
    Serial.print((unsigned long)numeroProducto);
    Serial.println(" de 5 cm o mas: apertura inmediata a 90 grados.");
  }

  return true;
}

// =====================================================
// RECEPCION ESP-NOW: CALLBACK CORTO, SIN ACTUAR MOTORES
// La maestra reenvia conteo/altura con este mismo formato.
// =====================================================
void recibirESPNow(const esp_now_recv_info_t *info,
                   const uint8_t *datos,
                   int longitud) {
  if (info == nullptr || info->src_addr == nullptr) return;
  // Aceptar paquetes solamente desde la MAC configurada de la maestra.
  if (memcmp(info->src_addr, MAC_MAESTRA, 6) != 0) return;
  if (longitud != sizeof(PaqueteESPNow)) return;

  PaqueteESPNow p;
  memcpy(&p, datos, sizeof(p));

  if (p.magic != MAGIC_ESPNOW) return;
  if (p.tipo != TIPO_CONTEO && p.tipo != TIPO_ALTURA) return;

  if (colaESPNow != nullptr) {
    // No bloquear dentro del callback de radio.
    xQueueSend(colaESPNow, &p, 0);
  }
}

// =====================================================
// PROCESAR MENSAJES RECIBIDOS EN EL LOOP PRINCIPAL
// =====================================================
void procesarMensajesESPNow() {
  if (colaESPNow == nullptr) return;

  PaqueteESPNow p;
  while (xQueueReceive(colaESPNow, &p, 0) == pdTRUE) {
    if (p.tipo == TIPO_CONTEO) {
      if (p.valor1 < 0) {
        Serial.println("[ESP-NOW] Conteo negativo rechazado.");
        continue;
      }

      uint32_t conteo = (uint32_t)p.valor1;

      // Primer paquete sincroniza el contador; evita crear
      // productos falsos si la maestra empieza con un conteo previo.
      if (!conteoInicializado) {
        ultimoConteoRecibido = conteo;
        conteoInicializado = true;
        Serial.print("[ESP-NOW] Conteo inicial sincronizado: ");
        Serial.println((unsigned long)conteo);
      } else {
        registrarNuevoConteo(conteo);
      }
    }

    else if (p.tipo == TIPO_ALTURA) {
      // La maestra debe codificar la altura en centesimas de cm:
      // 425 representa 4.25 cm.
      if (p.valor1 < 0 || p.valor1 > 100000) {
        Serial.println("[ESP-NOW] Altura fuera de rango; descartada.");
        continue;
      }

      float altura = ((float)p.valor1) / 100.0f;
      if (!isfinite(altura)) {
        Serial.println("[ESP-NOW] Altura invalida.");
        continue;
      }

      Serial.print("[HC-SR04] Altura recibida: ");
      Serial.print(altura, 2);
      Serial.println(" cm");

      if (cantidadIDs == 0) {
        Serial.println("[ERROR] Altura recibida sin producto pendiente.");
        continue;
      }

      uint32_t id = sacarID();
      Serial.print("[HC-SR04] Clasificando producto #");
      Serial.println((unsigned long)id);
      programarTrabajo(id, altura);
    }
  }
}

// =====================================================
// ¿HAY OBJETOS O ACTUACIONES PENDIENTES?
// =====================================================
bool hayProductosPendientes() {
  if (cantidadIDs > 0) return true;

  for (int i = 0; i < MAX_TRABAJOS; i++) {
    if (trabajos[i].activo) return true;
  }
  return false;
}

// =====================================================
// EJECUTAR ACTUACIONES SEGUN LA ALTURA
// =====================================================
void procesarTrabajosProgramados() {
  if (modoServo != SERVO_REPOSO) return;
  if (bloqueoPorFinalCarrera) return;

  // PRIORIDAD: OBJETOS MENORES DE 5 CM
  for (int i = 0; i < MAX_TRABAJOS; i++) {
    if (!trabajos[i].activo || trabajos[i].tipo != TRABAJO_PEQUENO) continue;

    uint32_t id = trabajos[i].numeroProducto;
    trabajos[i].activo = false;

    Serial.print("[SERVO] Apertura a 140 grados por altura del #");
    Serial.println((unsigned long)id);

    compuerta.write(ANGULO_PEQUENO);
    modoServo = SERVO_PEQUENO_ESPERA_FINAL;
    bandaActiva = true;
    return;
  }

  // OBJETOS DE 5 CM O MAS
  for (int i = 0; i < MAX_TRABAJOS; i++) {
    if (!trabajos[i].activo || trabajos[i].tipo != TRABAJO_GRANDE) continue;

    uint32_t id = trabajos[i].numeroProducto;
    trabajos[i].activo = false;

    Serial.print("[SERVO] Apertura inmediata a 90 grados por altura del #");
    Serial.println((unsigned long)id);

    compuerta.write(ANGULO_GRANDE);
    modoServo = SERVO_GRANDE_ABIERTO;
    inicioServoGrande = millis();
    temporizadorParadaActivo = true;
    inicioTemporizadorParada = millis();
    bandaActiva = true;

    Serial.println("[BANDA] Continua durante 25 s salvo que entre otro objeto.");
    return;
  }
}

// =====================================================
// FINAL DE CARRERA: CIERRE DE OBJETO PEQUEÑO
// =====================================================
void revisarFinalCarrera() {
  unsigned long ahora = millis();
  int lectura = digitalRead(FINAL_CARRERA_PIN);

  if (lectura != lecturaAnteriorFinal) {
    lecturaAnteriorFinal = lectura;
    ultimoCambioFinal = ahora;
  }

  if (ahora - ultimoCambioFinal >= ANTIRREBOTE_FINAL_MS) {
    estadoEstableFinal = lectura;
  }

  bool presionado = (estadoEstableFinal == LOW);
  if (presionado && !finalPresionadoAnterior) {
    if (modoServo == SERVO_PEQUENO_ESPERA_FINAL) {
      Serial.println("[FINAL] Final de carrera activado.");
      Serial.println("[FINAL] Deteniendo banda y cerrando servo.");

      bandaActiva = false;
      compuerta.write(ANGULO_REPOSO);
      modoServo = SERVO_REPOSO;
      bloqueoPorFinalCarrera = true;
      temporizadorParadaActivo = false;
    }
  }

  finalPresionadoAnterior = presionado;
}

// =====================================================
// CIERRE DEL SERVO GRANDE A LOS 13 SEGUNDOS
// =====================================================
void revisarCierreServoGrande() {
  if (modoServo != SERVO_GRANDE_ABIERTO) return;
  if (millis() - inicioServoGrande < TIEMPO_SERVO_GRANDE_MS) return;

  Serial.println("[SERVO] Tiempo cumplido: cerrando a 180 grados.");
  compuerta.write(ANGULO_REPOSO);
  modoServo = SERVO_REPOSO;
}

// =====================================================
// PARADA DE BANDA DESPUES DE 25 SEGUNDOS
// =====================================================
void revisarParadaBanda() {
  if (!temporizadorParadaActivo) return;

  if (!bandaActiva) {
    temporizadorParadaActivo = false;
    return;
  }

  if (millis() - inicioTemporizadorParada < TIEMPO_BANDA_MS) return;
  if (modoServo != SERVO_REPOSO) return;
  if (hayProductosPendientes()) return;

  Serial.println("[BANDA] 25 s sin nuevos objetos y sin trabajos pendientes.");
  Serial.println("[BANDA] Deteniendo banda.");
  bandaActiva = false;
  temporizadorParadaActivo = false;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  for (int i = 0; i < 4; i++) pinMode(pinesMotor[i], OUTPUT);
  apagarMotor();

  pinMode(FINAL_CARRERA_PIN, INPUT_PULLUP);
  compuerta.setPeriodHertz(50);
  compuerta.attach(SERVO_PIN, 500, 2400);
  compuerta.write(ANGULO_REPOSO);
  delay(500);

  lecturaAnteriorFinal = digitalRead(FINAL_CARRERA_PIN);
  estadoEstableFinal = lecturaAnteriorFinal;
  finalPresionadoAnterior = (estadoEstableFinal == LOW);
  ultimoCambioFinal = millis();

  for (int i = 0; i < MAX_TRABAJOS; i++) trabajos[i].activo = false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();
  delay(100);

  esp_err_t canalResultado = esp_wifi_set_channel(
    CANAL_ESPNOW,
    WIFI_SECOND_CHAN_NONE
  );
  if (canalResultado != ESP_OK) {
    Serial.printf("[ERROR] No se pudo fijar canal Wi-Fi: %d\n", (int)canalResultado);
    while (true) delay(1000);
  }

  colaESPNow = xQueueCreate(20, sizeof(PaqueteESPNow));
  if (colaESPNow == nullptr) {
    Serial.println("[ERROR] No se pudo crear la cola ESP-NOW.");
    while (true) delay(1000);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] No se pudo iniciar ESP-NOW.");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(recibirESPNow);

  Serial.println();
  Serial.println("======================================");
  Serial.println("OT-FORTRESS - ESP32-01");
  Serial.println("Comunicacion ESP-NOW con ESP32-04.");
  Serial.println("IR/conteo: arranque de banda.");
  Serial.println("Altura < 5 cm: servo a 140 grados.");
  Serial.println("Altura >= 5 cm: servo a 90 grados.");
  Serial.println("Servo grande: cierre tras 13 s.");
  Serial.println("Banda: parada tras 25 s sin nuevos objetos.");
  Serial.print("MAC propia: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Canal ESP-NOW: %d\n", CANAL_ESPNOW);
  Serial.println("======================================");

  BaseType_t resultado = xTaskCreate(
    tareaMotor,
    "TareaMotor",
    4096,
    nullptr,
    2,
    nullptr
  );

  if (resultado != pdPASS) {
    Serial.println("[ERROR] No se pudo iniciar la tarea del motor.");
  }
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  procesarMensajesESPNow();
  revisarFinalCarrera();
  revisarCierreServoGrande();
  procesarTrabajosProgramados();
  revisarParadaBanda();
  delay(1);
}
