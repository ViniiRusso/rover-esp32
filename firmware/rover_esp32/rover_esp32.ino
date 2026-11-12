/*
  Rover ESP32 + TB6612FNG, controlado pelo site (Vercel) via Wi-Fi + MQTT.

  LIGACOES (o firmware usa exatamente estas):
    Motor A:  GPIO 26 -> AIN1 | GPIO 27 -> AIN2 | GPIO 25 -> PWMA | AO1/AO2 -> motor A
    Motor B:  GPIO 14 -> BIN1 | GPIO 13 -> BIN2 | GPIO 33 -> PWMB | BO1/BO2 -> motor B
    3V3 do ESP32 -> VCC do driver
    STBY do driver -> VCC (sempre habilitado; por isso nao existe pino de STBY aqui)
    GND do ESP32, GND do driver e GND da plaquinha USB: todos juntos
    VBUS (+5 V) da plaquinha USB -> VM do driver

  BIBLIOTECA: PubSubClient, de Nick O'Leary (Ferramentas > Gerenciar Bibliotecas).
  SERIAL MONITOR: 115200 baud. Digite "ajuda" para ver os comandos.

  Funciona nos nucleos Arduino-ESP32 2.x e 3.x: a API de PWM certa e escolhida
  sozinha na compilacao (veja a secao "PWM").
*/

#include <WiFi.h>
#include <PubSubClient.h>

// ───────── 1. Wi-Fi: PREENCHA COM OS DADOS DA SUA REDE (so 2,4 GHz) ─────────
const char* WIFI_SSID  = "NOME_DA_SUA_REDE";
const char* WIFI_SENHA = "SENHA_DA_SUA_REDE";

// ───────── 2. MQTT: igual ao que o site usa ─────────
const char*    MQTT_HOST     = "broker.hivemq.com";
const uint16_t MQTT_PORTA    = 1883;
const char*    TOPICO_CMD    = "nfw-motores/6a21b1e8/cmd";
const char*    TOPICO_STATUS = "nfw-motores/6a21b1e8/status";

// ───────── 3. Pinos ─────────
const uint8_t PIN_AIN1 = 26;
const uint8_t PIN_AIN2 = 27;
const uint8_t PIN_PWMA = 25;
const uint8_t PIN_BIN1 = 14;
const uint8_t PIN_BIN2 = 13;
const uint8_t PIN_PWMB = 33;

// ───────── 4. Ajustes ─────────
const bool INVERTER_MOTOR_A     = false;  // true se o motor A girar ao contrario
const bool INVERTER_MOTOR_B     = false;  // true se o motor B girar ao contrario
const bool MOTOR_A_E_O_ESQUERDO = true;   // false se o motor A estiver do lado direito

const uint32_t PWM_FREQ_HZ = 20000;  // acima do que o ouvido escuta; o TB6612FNG aceita ate 100 kHz
const uint8_t  PWM_BITS    = 8;      // velocidade de 0 a 255, igual ao site
const uint8_t  VEL_PADRAO  = 200;

const uint32_t TEMPO_SEM_SINAL_MS = 600;    // o site repete o comando a cada 250 ms; sem isso, para
const uint32_t TEMPO_SERIAL_MS    = 3000;   // comando digitado no Serial vale por 3 s
const uint32_t TEMPO_MEDIR_MS     = 30000;  // comando "medir" segura os motores ligados por 30 s

// ───────── Estado ─────────
enum Sentido : uint8_t { PARADO, FRENTE, TRAS };

struct Motor {
  const char* nome;
  const char* rotIn1;
  const char* rotIn2;
  const char* rotPwm;
  uint8_t pinIn1, pinIn2, pinPwm;
  uint8_t canal;      // so usado na API de PWM antiga (nucleo 2.x)
  bool    invertido;
  Sentido sentido;
  uint8_t duty;
};

Motor motorA = { "A", "AIN1", "AIN2", "PWMA", PIN_AIN1, PIN_AIN2, PIN_PWMA, 0, INVERTER_MOTOR_A, PARADO, 0 };
Motor motorB = { "B", "BIN1", "BIN2", "PWMB", PIN_BIN1, PIN_BIN2, PIN_PWMB, 1, INVERTER_MOTOR_B, PARADO, 0 };

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

uint8_t  velocidade       = VEL_PADRAO;
uint32_t ultimoComandoMs  = 0;
uint32_t limiteSemSinalMs = TEMPO_SEM_SINAL_MS;

// ───────── PWM ─────────
// Nucleo 3.x: ledcAttach(pino, freq, bits) e ledcWrite(pino, duty).
// Nucleo 2.x: ledcSetup(canal, freq, bits), ledcAttachPin(pino, canal) e ledcWrite(canal, duty).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define PWM_API_NOVA 1
#else
  #define PWM_API_NOVA 0
#endif

void pwmEscrever(Motor& m, uint8_t duty) {
#if PWM_API_NOVA
  ledcWrite(m.pinPwm, duty);
#else
  ledcWrite(m.canal, duty);
#endif
}

bool pwmIniciar(Motor& m) {
  // pinMode vem ANTES do ledc: no nucleo 3.x, um pinMode depois do ledcAttach desfaz o PWM.
  pinMode(m.pinPwm, OUTPUT);
  digitalWrite(m.pinPwm, LOW);
#if PWM_API_NOVA
  bool ok = ledcAttach(m.pinPwm, PWM_FREQ_HZ, PWM_BITS);
#else
  bool ok = ledcSetup(m.canal, PWM_FREQ_HZ, PWM_BITS) > 0;
  ledcAttachPin(m.pinPwm, m.canal);
#endif
  pwmEscrever(m, 0);
  return ok;
}

// ───────── Motores ─────────
const char* nomeSentido(Sentido s) {
  return s == FRENTE ? "FRENTE" : s == TRAS ? "TRAS" : "PARADO";
}

// Os valores de IN1/IN2 sao lidos de volta do proprio pino, nao da memoria.
void imprimirMotor(Motor& m) {
  Serial.printf("[PINOS] Motor %s %-6s | %s(GPIO%u)=%d  %s(GPIO%u)=%d  %s(GPIO%u)=%u/255\n",
                m.nome, nomeSentido(m.sentido),
                m.rotIn1, m.pinIn1, digitalRead(m.pinIn1),
                m.rotIn2, m.pinIn2, digitalRead(m.pinIn2),
                m.rotPwm, m.pinPwm, m.duty);
}

void imprimirPinos() {
  imprimirMotor(motorA);
  imprimirMotor(motorB);
}

// Tabela do TB6612FNG:
//   IN1=1 IN2=0 + PWM -> gira num sentido
//   IN1=0 IN2=1 + PWM -> gira no outro
//   IN1=0 IN2=0, PWM=0 -> motor desligado (solto)
// Devolve true se algum pino mudou.
bool aplicarMotor(Motor& m, Sentido s, uint8_t vel) {
  uint8_t duty = (s == PARADO) ? 0 : vel;
  if (s == m.sentido && duty == m.duty) return false;

  if (s != m.sentido) pwmEscrever(m, 0);  // corta a forca antes de trocar o sentido

  Sentido fisico = s;
  if (m.invertido && s != PARADO) fisico = (s == FRENTE) ? TRAS : FRENTE;
  digitalWrite(m.pinIn1, fisico == FRENTE ? HIGH : LOW);
  digitalWrite(m.pinIn2, fisico == TRAS ? HIGH : LOW);
  pwmEscrever(m, duty);

  m.sentido = s;
  m.duty = duty;
  return true;
}

void mover(Sentido a, Sentido b, uint8_t vel) {
  bool mudouA = aplicarMotor(motorA, a, vel);
  bool mudouB = aplicarMotor(motorB, b, vel);
  if (mudouA || mudouB) imprimirPinos();
}

void pararTudo() {
  mover(PARADO, PARADO, 0);
}

// ───────── Diagnostico ─────────
const char* motivoWifi() {
  switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL:   return "rede nao encontrada: confira o nome e se e 2,4 GHz";
    case WL_CONNECT_FAILED:  return "falha ao conectar: confira a senha";
    case WL_CONNECTION_LOST: return "conexao perdida";
    default:                 return "tentando";
  }
}

const char* motivoMqtt(int estado) {
  switch (estado) {
    case -4: return "o broker nao respondeu a tempo";
    case -3: return "conexao perdida";
    case -2: return "nao consegui abrir conexao com o broker";
    case -1: return "desconectado";
    default: return "recusado pelo broker";
  }
}

bool wifiPreenchido() {
  return strcmp(WIFI_SSID, "NOME_DA_SUA_REDE") != 0;
}

void imprimirStatus() {
  if (WiFi.status() == WL_CONNECTED) Serial.printf("[STATUS] WiFi conectado (%d dBm)", (int)WiFi.RSSI());
  else                               Serial.print("[STATUS] WiFi DESCONECTADO");
  Serial.printf(" | MQTT %s | Motor A %s | Motor B %s | velocidade %u/255\n",
                mqtt.connected() ? "conectado" : "DESCONECTADO",
                nomeSentido(motorA.sentido), nomeSentido(motorB.sentido), velocidade);
}

void imprimirAjuda() {
  Serial.println();
  Serial.println("Comandos (digite aqui ou envie pelo site):");
  Serial.println("  frente | tras | esquerda | direita | parar    atalhos: f t e d p");
  Serial.println("  v200             muda a velocidade (0 a 255)");
  Serial.println("  v200 1f 2p       formato do site: motor 1 ou 2 + f (frente), t (tras), p (parado)");
  Serial.println("  s                para tudo");
  Serial.println("So aqui pelo Serial:");
  Serial.println("  teste            gira cada motor para frente e para tras, um de cada vez");
  Serial.println("  medir            liga os dois motores em 100% por 30 s, para medir com multimetro");
  Serial.println("  status           mostra WiFi, MQTT e motores");
  Serial.println("Movimento pedido pelo Serial dura 3 s e para sozinho.");
  Serial.println();
}

// Gira cada motor sozinho, sem depender de Wi-Fi nem do site.
void autoteste() {
  struct Passo { const char* nome; Sentido a; Sentido b; };
  const Passo passos[] = {
    { "Motor A para frente", FRENTE, PARADO },
    { "Motor A para tras",   TRAS,   PARADO },
    { "Motor B para frente", PARADO, FRENTE },
    { "Motor B para tras",   PARADO, TRAS   },
  };
  Serial.println("[TESTE] Tire as rodas do chao. Cada passo dura 1,5 s, em 100% de velocidade.");
  for (const Passo& p : passos) {
    Serial.printf("[TESTE] %s\n", p.nome);
    mover(p.a, p.b, 255);
    delay(1500);
    pararTudo();
    delay(400);
  }
  Serial.println("[TESTE] Fim. Se os pinos apareceram em 1 e o motor nao girou, o problema e eletrico:");
  Serial.println("[TESTE] digite medir e confira VM, STBY e GND com o multimetro.");
}

// Liga os dois motores em 100% e segura, para dar tempo de medir.
void modoMedir() {
  Serial.println("[MEDIR] Dois motores ligados em 100% por 30 s. Digite parar para desligar antes.");
  limiteSemSinalMs = TEMPO_MEDIR_MS;
  ultimoComandoMs = millis();
  mover(FRENTE, FRENTE, 255);
  Serial.println("[MEDIR] Multimetro em tensao continua, ponta preta no GND. Valores esperados:");
  Serial.println("[MEDIR]   pino que aparece acima como 1 ou 255/255: perto de 3,3 V. Pino em 0: perto de 0 V");
  Serial.println("[MEDIR]   VM do driver: perto de 5 V");
  Serial.println("[MEDIR]   VCC do driver: perto de 3,3 V");
  Serial.println("[MEDIR]   STBY do driver: perto de 3,3 V (se der 0 V, o driver esta dormindo)");
  Serial.println("[MEDIR]   entre AO1 e AO2, e entre BO1 e BO2: perto do valor de VM");
}

// ───────── Comandos (site e Serial usam a mesma funcao) ─────────
void tratarComando(String msg, bool viaSerial) {
  msg.trim();
  msg.toLowerCase();
  if (msg.length() == 0) return;

  // O site repete o mesmo comando 4 vezes por segundo: mostra de novo so a cada 2 s.
  static String   ultimaMsg;
  static uint32_t ultimoLogMs = 0;
  if (viaSerial || msg != ultimaMsg || millis() - ultimoLogMs >= 2000) {
    Serial.printf("[%s] recebido: \"%s\"\n", viaSerial ? "SERIAL" : "MQTT", msg.c_str());
    ultimaMsg = msg;
    ultimoLogMs = millis();
  }

  if (viaSerial) {
    if (msg == "ajuda" || msg == "?") { imprimirAjuda(); return; }
    if (msg == "status") { imprimirStatus(); return; }
    if (msg == "teste")  { autoteste(); return; }
    if (msg == "medir")  { modoMedir(); return; }
  }

  Sentido a = motorA.sentido;
  Sentido b = motorB.sentido;
  uint8_t vel = velocidade;
  Sentido& esq = MOTOR_A_E_O_ESQUERDO ? a : b;
  Sentido& dir = MOTOR_A_E_O_ESQUERDO ? b : a;

  int ini = 0;
  while (ini < (int)msg.length()) {
    int fim = msg.indexOf(' ', ini);
    if (fim < 0) fim = msg.length();
    String t = msg.substring(ini, fim);
    ini = fim + 1;
    if (t.length() == 0) continue;

    if (t.length() > 1 && t[0] == 'v' && isDigit(t[1])) {
      vel = (uint8_t)constrain(t.substring(1).toInt(), 0L, 255L);
    }
    else if (t == "s" || t == "p" || t == "parar" || t == "stop") { a = PARADO; b = PARADO; }
    else if (t == "1f") a = FRENTE;
    else if (t == "1t") a = TRAS;
    else if (t == "1p") a = PARADO;
    else if (t == "2f") b = FRENTE;
    else if (t == "2t") b = TRAS;
    else if (t == "2p") b = PARADO;
    else if (t == "frente" || t == "f")              { a = FRENTE; b = FRENTE; }
    else if (t == "tras" || t == "re" || t == "t")   { a = TRAS;   b = TRAS; }
    else if (t == "esquerda" || t == "e")            { esq = TRAS;   dir = FRENTE; }  // gira no lugar
    else if (t == "direita" || t == "d")             { esq = FRENTE; dir = TRAS; }
    else {
      Serial.printf("[ERRO] nao entendi \"%s\". Comando ignorado. Digite ajuda.\n", t.c_str());
      return;
    }
  }

  if (vel != velocidade) Serial.printf("[VEL] velocidade agora %u/255\n", vel);
  velocidade = vel;
  limiteSemSinalMs = viaSerial ? TEMPO_SERIAL_MS : TEMPO_SEM_SINAL_MS;
  ultimoComandoMs = millis();
  mover(a, b, vel);
}

void aoReceberMqtt(char* topico, byte* dados, unsigned int tamanho) {
  (void)topico;
  String msg;
  for (unsigned int i = 0; i < tamanho && i < 64; i++) msg += (char)dados[i];
  tratarComando(msg, false);
}

// Le uma linha do Serial Monitor. Funciona mesmo com "Sem final de linha" selecionado.
void lerSerial() {
  static String   linha;
  static uint32_t ultimoCharMs = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    ultimoCharMs = millis();
    if (c == '\n' || c == '\r') {
      if (linha.length() > 0) { tratarComando(linha, true); linha = ""; }
    } else if (linha.length() < 64) {
      linha += c;
    }
  }
  if (linha.length() > 0 && millis() - ultimoCharMs > 300) {
    tratarComando(linha, true);
    linha = "";
  }
}

// ───────── Wi-Fi e MQTT ─────────
void cuidarDoWifi() {
  if (!wifiPreenchido()) return;

  static bool     estavaConectado  = false;
  static uint32_t ultimoAvisoMs    = 0;
  static uint32_t ultimaTentativaMs = 0;

  bool conectado = (WiFi.status() == WL_CONNECTED);
  if (conectado && !estavaConectado) {
    Serial.printf("[WIFI] conectado em \"%s\". IP %s, sinal %d dBm\n",
                  WIFI_SSID, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  }
  if (!conectado && estavaConectado) {
    Serial.println("[WIFI] caiu. Parando os motores.");
    pararTudo();
  }
  if (!conectado && millis() - ultimoAvisoMs >= 5000) {
    ultimoAvisoMs = millis();
    Serial.printf("[WIFI] ainda sem conexao em \"%s\" (%s)\n", WIFI_SSID, motivoWifi());
  }
  if (!conectado && millis() - ultimaTentativaMs >= 15000) {
    ultimaTentativaMs = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
  }
  estavaConectado = conectado;
}

void cuidarDoMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }

  static bool     jaTentou = false;
  static uint32_t ultimaTentativaMs = 0;
  if (jaTentou && millis() - ultimaTentativaMs < 3000) return;
  jaTentou = true;
  ultimaTentativaMs = millis();

  // connect() pode travar por alguns segundos: nenhum motor fica ligado enquanto isso.
  pararTudo();

  char id[24];
  snprintf(id, sizeof(id), "rover-%08x", (unsigned)ESP.getEfuseMac());
  Serial.printf("[MQTT] conectando em %s:%u ...\n", MQTT_HOST, MQTT_PORTA);

  // "offline" fica registrado no broker e e publicado sozinho se o ESP32 sumir.
  if (mqtt.connect(id, TOPICO_STATUS, 0, true, "offline")) {
    mqtt.publish(TOPICO_STATUS, "online", true);
    mqtt.subscribe(TOPICO_CMD);
    Serial.printf("[MQTT] conectado. Ouvindo \"%s\"\n", TOPICO_CMD);
  } else {
    Serial.printf("[MQTT] falhou (codigo %d: %s). Tento de novo em 3 s.\n", mqtt.state(), motivoMqtt(mqtt.state()));
  }
}

// Se os comandos pararem de chegar com motor ligado, desliga.
void vigiarSinal() {
  bool andando = (motorA.sentido != PARADO) || (motorB.sentido != PARADO);
  if (andando && millis() - ultimoComandoMs > limiteSemSinalMs) {
    Serial.printf("[SEGURANCA] %lu ms sem comando. Parando os motores.\n", (unsigned long)limiteSemSinalMs);
    pararTudo();
  }
}

void avisoPeriodico() {
  static uint32_t ultimoMs = 0;
  if (millis() - ultimoMs < 10000) return;
  ultimoMs = millis();
  imprimirStatus();
}

// ───────── setup / loop ─────────
void setup() {
  // Motores desligados antes de qualquer outra coisa.
  const uint8_t pinosDeSentido[] = { PIN_AIN1, PIN_AIN2, PIN_BIN1, PIN_BIN2 };
  for (uint8_t pino : pinosDeSentido) {
    pinMode(pino, OUTPUT);
    digitalWrite(pino, LOW);
  }
  bool pwmAok = pwmIniciar(motorA);
  bool pwmBok = pwmIniciar(motorB);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Rover ESP32 + TB6612FNG ===");
#ifdef ESP_ARDUINO_VERSION_MAJOR
  Serial.printf("Nucleo Arduino-ESP32 %d.%d.%d, API de PWM: %s\n",
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
                PWM_API_NOVA ? "nova (ledcAttach)" : "antiga (ledcSetup + ledcAttachPin)");
#else
  Serial.println("Nucleo Arduino-ESP32 anterior a 2.0, API de PWM antiga");
#endif
  Serial.printf("PWM %lu Hz, %u bits: PWMA(GPIO%u) %s, PWMB(GPIO%u) %s\n",
                (unsigned long)PWM_FREQ_HZ, PWM_BITS,
                PIN_PWMA, pwmAok ? "ok" : "FALHOU",
                PIN_PWMB, pwmBok ? "ok" : "FALHOU");
  imprimirPinos();
  Serial.println("Digite ajuda para ver os comandos. Digite teste para girar os motores sem Wi-Fi.");

  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setCallback(aoReceberMqtt);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);

  if (wifiPreenchido()) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // resposta mais rapida aos comandos
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
    Serial.printf("[WIFI] conectando em \"%s\" ...\n", WIFI_SSID);
  } else {
    Serial.println("[WIFI] ATENCAO: preencha WIFI_SSID e WIFI_SENHA no topo do codigo.");
    Serial.println("[WIFI] Sem isso o site nao funciona, mas os comandos pelo Serial funcionam.");
  }
}

void loop() {
  lerSerial();
  cuidarDoWifi();
  cuidarDoMqtt();
  vigiarSinal();
  avisoPeriodico();
}
