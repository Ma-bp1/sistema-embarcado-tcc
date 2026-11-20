// CHRONOBOX - Caixa organizadora inteligente de remedios

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Wire.h>
#include <RTClib.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <esp_system.h>

// ===================== CONFIGURACOES =====================

// WiFi (somente 2.4 GHz)
const char* WIFI_SSID  = "VIVOFIBRA-F884";
const char* WIFI_SENHA = "c662faf884";

// Firebase
const char* API_KEY    = "AIzaSyCdH_Y6ubkcqECLsWJ-DBAyYA7FtqggRRw";
const char* PROJECT_ID = "projeto-1-c7102";

// Conta criada em Firebase > Authentication, so para a caixa
const char* DEVICE_EMAIL = "admin@gmail.com";
const char* DEVICE_SENHA = "senha123";

// Pinos
const int NUM_SLOTS = 6;
const int PINOS_LED[NUM_SLOTS]   = {4, 2, 15, 25, 26, 27};
const int PINOS_SERVO[NUM_SLOTS] = {5, 17, 16, 14, 33, 13};
const int PINO_BUZZER          = 32;
const int PINO_BOTAO_POWER     = 18;
const int PINO_BOTAO_CONFIRMAR = 19;

// Botoes:
// true  = o botao liga o pino ao 3V3 (usa INPUT_PULLDOWN)
// false = o botao liga o pino ao GND (usa INPUT_PULLUP)
const bool BOTAO_ATIVO_ALTO = false;

// Servos
const int ANGULO_FECHADO = 0;
const int ANGULO_ABERTO  = 180;
// true  = solta o servo (detach) depois de mover: evita pico/aquecimento e reset por queda de tensao
// false = mantem o servo energizado segurando a posicao
const bool SERVO_DESANEXAR = true;
const unsigned long TEMPO_MOVER_MS = 700;   // tempo ate o servo chegar na posicao
const unsigned long ESCALONAR_MS   = 250;   // atraso entre um servo e outro (abrir/fechar varios)

// Fuso horario do Brasil (UTC-3)
const long FUSO_SEGUNDOS = -3L * 3600;

// Tempos
const unsigned long TEMPO_ABERTO_MS = 60000;  // alerta expira sozinho em 1 min
const unsigned long TEMPO_FECHAR_POS_CONFIRMAR_MS = 10000;  // depois de confirmar: fecha em 10 s
const unsigned long JANELA_CLIQUES  = 5000;   // tempo maximo entre cliques do combo
const unsigned long TEMPO_SEGURAR   = 2000;   // tempo segurando o 4o clique
const unsigned long TEMPO_POWER_MS  = 1500;   // segurar o botao power para ligar/desligar
const unsigned long DEBOUNCE_MS     = 30;

unsigned long fechaPosConfirmarEm[NUM_SLOTS] = {0};  // 0 = nenhum fechamento pendente

// Intervalos de rede
const unsigned long INTERVALO_NORMAL_MS = 3000;   // consulta ao Firebase sem alerta
const unsigned long INTERVALO_ALERTA_MS = 700;    // consulta ao Firebase com alerta tocando
const unsigned long INTERVALO_STATUS_MS = 10000;  // "estou vivo" (lastSeen)

const int ESTOQUE_MINIMO_PADRAO = 2;
const int ADIAR_PADRAO_MIN = 5;

// ===================== TIPOS =====================

enum TipoEvento : uint8_t {
  EV_TOMADO = 1,
  EV_ADIADO = 2,
  EV_PERDIDO = 3
};

struct EventoDose {
  uint32_t horario;
  uint8_t slot;
  uint8_t tipo;
};

enum TipoCmd : uint8_t { CMD_NENHUM = 0, CMD_CONFIRMAR = 1, CMD_ADIAR = 2, CMD_PERDIDO = 3 };
struct Comando { uint8_t tipo; int8_t slot; uint16_t minutos; };
struct CmdEstoque { uint8_t slot; uint16_t qtd; uint32_t ver; };   // ajuste de estoque vindo do app

// ===================== VARIAVEIS =====================

LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;
Servo servos[NUM_SLOTS];
WiFiClientSecure clienteFirestore;   // conexao reaproveitada (keep-alive)

SemaphoreHandle_t mtx;       // protege agenda, agendaAt e estoque
QueueHandle_t filaDoses;     // loop -> rede: doses confirmadas
QueueHandle_t filaCmd;       // rede -> loop: comandos vindos do app
QueueHandle_t filaEstoque;   // rede -> loop: ajustes manuais de estoque feitos no app

struct Trava {
  Trava()  { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Trava() { xSemaphoreGive(mtx); }
};

JsonDocument agenda;
String agendaAt = "";
String boxId = "";

// usadas somente pela tarefa de rede
String idToken = "";
String resposta = "";
unsigned long tokenMs = 0;
String ultimoCmd = "";
bool cmdInicializado = false;
bool statusUrgente = false;
bool horaSincronizada = false;

// compartilhadas (volatile)
volatile bool cfgSom = true;   // alerta sonoro ligado
volatile bool cfgLuz = true;   // alerta luminoso ligado
volatile bool sistemaLigado = true;
volatile bool estoqueMudou = false;
volatile bool agendaNovaFlag = false;
volatile bool ajustarRtcFlag = false;
volatile uint32_t ntpLocalUnix = 0;
volatile uint32_t unixLocal = 0;      // hora local (RTC), atualizada pelo loop
volatile bool existeAlerta = false;
volatile int bateriaPct = -1;
volatile int bateriaMv  = 0;

// Servos (agendador nao bloqueante)
bool servoPendente[NUM_SLOTS] = {false};
int servoAlvo[NUM_SLOTS] = {0};
unsigned long servoInicio[NUM_SLOTS] = {0};
unsigned long servoSolta[NUM_SLOTS] = {0};

// Alertas (um por slot)
bool alertaAtivo[NUM_SLOTS]  = {false};
bool alertaLed[NUM_SLOTS]    = {false};
bool alertaBuzzer[NUM_SLOTS] = {false};
String alertaNome[NUM_SLOTS];
int alertaDose[NUM_SLOTS] = {0};
int alertaH[NUM_SLOTS];               // horario da agenda que disparou (-1 = adiamento)
int alertaM[NUM_SLOTS];
unsigned long fechaEm[NUM_SLOTS] = {0};
uint32_t adiaUnix[NUM_SLOTS] = {0};   // quando o alerta adiado volta a tocar (0 = nenhum)
// Evita disparar duas vezes a mesma dose (importante com a janela de antecedência)
uint32_t disparos[32] = {0};
int idxDisparo = 0;

int slotMostrado = -1;
unsigned long ultimaTroca = 0;
unsigned long ultimoPisca = 0;
bool estadoPisca = false;

bool todosAbertos = false;
int ultimoMinuto = -1;
int ultimoSegundoTela = -1;
unsigned long mensagemAte = 0;
unsigned long ultimaLeituraRtc = 0;

// Combo do botao confirmar (3 cliques + 4o segurado)
int cliques = 0;
unsigned long ultimoClique = 0;
unsigned long inicioSegurar = 0;
bool segurou = false;

// Estoque
int quantidadeAtual[NUM_SLOTS] = {0};
int estoqueMinimo[NUM_SLOTS];
String nomeConhecido[NUM_SLOTS];
int consumoPorDose[NUM_SLOTS];            // unidades descontadas por dose (medStockConsumption)
uint32_t estqVer[NUM_SLOTS] = {0};        // ultima versao de ajuste do app aplicada (salva na flash)
bool slotNaAgenda[NUM_SLOTS] = {false};   // true = o compartimento existe na agenda atual

// ===================== PROTOTIPOS =====================
// Funcoes com argumentos padrao: declaradas aqui (e SEM o padrao na definicao)
// para nao conflitar com os prototipos automaticos da Arduino IDE.
void enviarStatus(bool standby = false);
void iniciarAlerta(int i, String nome, int dose, bool usaLed, bool usaBuzzer, bool daAgenda, int hItem = -1, int mItem = -1);
void sincronizarEstoque(bool aplicarReabastecimento = true);

// ===================== DISPAROS =====================

bool jaDisparou(uint32_t k) {
  for (int i = 0; i < 32; i++) if (disparos[i] == k) return true;
  return false;
}
void marcarDisparo(uint32_t k) {
  disparos[idxDisparo] = k;
  idxDisparo = (idxDisparo + 1) % 32;
}

// ===================== HELPER DE EVENTOS =====================

void emitirEvento(int i, uint8_t tipo) {
  EventoDose ev{ (uint32_t)unixLocal, (uint8_t)i, tipo };
  xQueueSend(filaDoses, &ev, 0);
}

// ===================== BUZZER / BOTOES =====================

// Modulo "low level trigger": LOW liga, HIGH desliga
void ligarBuzzer()    { digitalWrite(PINO_BUZZER, LOW); }
void desligarBuzzer() { digitalWrite(PINO_BUZZER, HIGH); }

bool lerBotao(int pino) {
  return digitalRead(pino) == (BOTAO_ATIVO_ALTO ? HIGH : LOW);
}

// ===================== SERVOS =====================

void moverServo(int i, int angulo, unsigned long atraso) {
  servoAlvo[i] = angulo;
  servoInicio[i] = millis() + atraso;
  servoPendente[i] = true;
}

void atualizarServos() {
  unsigned long agoraMs = millis();
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (servoPendente[i] && (long)(agoraMs - servoInicio[i]) >= 0) {
      if (!servos[i].attached()) {
        servos[i].setPeriodHertz(50);
        servos[i].attach(PINOS_SERVO[i], 500, 2400);
      }
      servos[i].write(servoAlvo[i]);
      servoPendente[i] = false;
      servoSolta[i] = agoraMs + TEMPO_MOVER_MS;
    }
    if (SERVO_DESANEXAR && !servoPendente[i] && servoSolta[i] != 0 &&
        (long)(agoraMs - servoSolta[i]) >= 0) {
      servos[i].detach();
      servoSolta[i] = 0;
    }
  }
}

// ===================== MEMORIA (LittleFS) =====================
// Somente a tarefa principal (loop/setup) mexe no LittleFS.

void salvarAgendaLocal() {
  JsonDocument doc;
  doc["agendaAt"] = agendaAt;
  doc["itens"] = agenda;
  File f = LittleFS.open("/agenda.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

// devolve true se havia uma agenda salva e ela foi carregada
bool carregarAgendaLocal() {
  if (!LittleFS.exists("/agenda.json")) return false;
  File f = LittleFS.open("/agenda.json", "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, f);
  f.close();
  if (erro) return false;
  agendaAt = doc["agendaAt"].as<String>();
  agenda = doc["itens"];
  return true;
}

void salvarEstoqueLocal() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < NUM_SLOTS; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["s"] = i;
    o["nome"] = nomeConhecido[i];
    o["qtd"] = quantidadeAtual[i];
    o["min"] = estoqueMinimo[i];
    o["c"] = consumoPorDose[i];
    o["v"] = estqVer[i];
  }
  File f = LittleFS.open("/estoque.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void carregarEstoqueLocal() {
  if (!LittleFS.exists("/estoque.json")) return;
  File f = LittleFS.open("/estoque.json", "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, f);
  f.close();
  if (erro) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    int i = o["s"] | -1;
    if (i < 0 || i >= NUM_SLOTS) continue;
    nomeConhecido[i]   = o["nome"].as<String>();
    quantidadeAtual[i] = o["qtd"] | 0;
    estoqueMinimo[i]   = o["min"] | ESTOQUE_MINIMO_PADRAO;
    consumoPorDose[i]  = o["c"] | 1;
    estqVer[i]         = o["v"] | (uint32_t)0;
  }
}

// ===================== ESTOQUE =====================

// quantas doses ainda dá para tomar (mesma conta do app)
int dosesRestantes(int i) {
  int c = (consumoPorDose[i] > 0) ? consumoPorDose[i] : 1;
  return quantidadeAtual[i] / c;
}

// So existe estoque baixo para compartimento que ainda esta na agenda
bool estoqueBaixo(int i) {
  return slotNaAgenda[i] && nomeConhecido[i] != "" && dosesRestantes(i) <= estoqueMinimo[i];
}

// Reconcilia o estoque com a agenda atual. Chamar com o mutex travado.
// aplicarReabastecimento = false no boot: o "resetQtd" salvo na agenda local
// nao pode zerar o estoque a cada reinicio.
void sincronizarEstoque(bool aplicarReabastecimento) {
  bool presente[NUM_SLOTS] = {false};

  for (JsonObject item : agenda.as<JsonArray>()) {
    int slot = item["s"] | 0;
    if (slot < 1 || slot > NUM_SLOTS) continue;
    int i = slot - 1;
    presente[i] = true;

    String nome = item["n"] | "";
    if (nome == "") nome = "Compartimento " + String(slot);

    estoqueMinimo[i] = item["min"] | ESTOQUE_MINIMO_PADRAO;

    // consumo por dose: "c" (medStockConsumption). Se o app ainda nao mandar, usa "d".
    int c = item["c"] | (item["d"] | 1);
    consumoPorDose[i] = (c > 0) ? c : 1;

    bool reabastecer = aplicarReabastecimento && (item["resetQtd"] | false);

    // So aceita a quantidade da agenda se o remedio e novo naquele slot
    // ou se o app pediu reabastecimento (resetQtd = true).
    // Ajustes manuais na tela de estoque chegam por outro caminho (processarEstoqueApp).
    if (nomeConhecido[i] != nome || reabastecer) {
      nomeConhecido[i] = nome;
      quantidadeAtual[i] = item["q"] | 0;
      Serial.printf("Estoque do slot %d: %s, %d unidades (%d doses).\n",
                    slot, nome.c_str(), quantidadeAtual[i], dosesRestantes(i));
    }
  }

  // Slots que nao estao mais na agenda = medicamento deletado: limpa tudo
  for (int i = 0; i < NUM_SLOTS; i++) {
    slotNaAgenda[i] = presente[i];
    if (presente[i]) continue;
    if (nomeConhecido[i] == "" && quantidadeAtual[i] == 0) continue;
    Serial.printf("Slot %d removido pelo app: estoque limpo.\n", i + 1);
    nomeConhecido[i]   = "";
    quantidadeAtual[i] = 0;
    estoqueMinimo[i]   = ESTOQUE_MINIMO_PADRAO;
    consumoPorDose[i]  = 1;
  }

  salvarEstoqueLocal();
  estoqueMudou = true;
}

// ===================== RELOGIO =====================

DateTime horaAtual() { return DateTime((uint32_t)unixLocal); }

void atualizarRelogio() {
  if (millis() - ultimaLeituraRtc >= 200) {
    ultimaLeituraRtc = millis();
    unixLocal = rtc.now().unixtime();
  }
}

// Roda na tarefa de rede: so pega a hora da internet. O RTC e ajustado pelo loop.
void sincronizarHoraRede() {
  configTime(FUSO_SEGUNDOS, 0, "pool.ntp.org", "time.google.com");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    DateTime dt(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    ntpLocalUnix = dt.unixtime();
    ajustarRtcFlag = true;
    horaSincronizada = true;
    Serial.println("Hora sincronizada pela internet.");
  } else {
    Serial.println("Nao foi possivel sincronizar a hora (usando o RTC).");
  }
}

// ===================== FIREBASE (tarefa de rede) =====================

String docUrl() {
  return String("https://firestore.googleapis.com/v1/projects/") + PROJECT_ID +
         "/databases/(default)/documents/boxes/" + boxId;
}

int chamarFirebase(WiFiClientSecure &cli, const char* metodo, const String &url, const String &corpo) {
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(6000);
  http.setConnectTimeout(5000);
  if (!http.begin(cli, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  if (idToken != "") http.addHeader("Authorization", "Bearer " + idToken);
  int code = http.sendRequest(metodo, corpo);
  resposta = (code > 0) ? http.getString() : "";
  http.end();
  if (code <= 0) cli.stop();   // conexao quebrada: a proxima abre outra
  return code;
}

bool fazerLogin() {
  idToken = "";
  JsonDocument body;
  body["email"] = DEVICE_EMAIL;
  body["password"] = DEVICE_SENHA;
  body["returnSecureToken"] = true;
  String corpo;
  serializeJson(body, corpo);

  WiFiClientSecure clienteLogin;
  clienteLogin.setInsecure();
  int code = chamarFirebase(clienteLogin, "POST",
    String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + API_KEY, corpo);

  if (code == 200) {
    JsonDocument r;
    deserializeJson(r, resposta);
    idToken = r["idToken"].as<String>();
    tokenMs = millis();
    Serial.println("Login no Firebase OK");
    return true;
  }
  Serial.printf("Login falhou: %d\n", code);
  Serial.println(resposta);
  return false;
}

void enviarStatus(bool standby) {
  String ack;
  { Trava t; ack = agendaAt; }

  String nome = String("projects/") + PROJECT_ID +
                "/databases/(default)/documents/boxes/" + boxId;

  JsonDocument doc;
  JsonObject w  = doc["writes"].add<JsonObject>();
  JsonObject up = w["update"].to<JsonObject>();
  up["name"] = nome;
  JsonObject f = up["fields"].to<JsonObject>();
  f["agendaAck"]["stringValue"] = ack;
  f["cmdAck"]["stringValue"]    = ultimoCmd;
  f["estado"]["stringValue"]    = standby ? "standby" : "ligada";
  f["bateria"]["integerValue"]  = String(bateriaPct);
  f["bateriaMv"]["integerValue"] = String(bateriaMv);

  JsonArray mask = w["updateMask"]["fieldPaths"].to<JsonArray>();
  mask.add("agendaAck"); mask.add("cmdAck"); mask.add("estado");
  mask.add("bateria");   mask.add("bateriaMv");

  JsonObject tr = w["updateTransforms"].add<JsonObject>();
  tr["fieldPath"] = "lastSeen";
  tr["setToServerValue"] = "REQUEST_TIME";   // hora do servidor

  String corpo; serializeJson(doc, corpo);
  String url = String("https://firestore.googleapis.com/v1/projects/") + PROJECT_ID +
               "/databases/(default)/documents:commit";
  int code = chamarFirebase(clienteFirestore, "POST", url, corpo);
  if (code == 401 || code == 403) idToken = "";
  if (code != 200) Serial.printf("Status: HTTP %d\n", code);
}

bool enviarEvento(const EventoDose &ev) {
  const char* nomes[] = { "", "tomado", "adiado", "perdido" };
  if (ev.tipo < 1 || ev.tipo > 3) return true;   // evento invalido: descarta

  JsonDocument doc;
  JsonObject fl = doc["fields"].to<JsonObject>();

  fl["eventoId"]["stringValue"]       = String(ev.horario) + "-" + String(ev.slot) + "-" + String(ev.tipo);
  fl["eventoTipo"]["stringValue"]     = nomes[ev.tipo];
  fl["eventoSlot"]["integerValue"]    = String(ev.slot + 1);   // 1..6 para o app
  fl["eventoHorario"]["integerValue"] = String(ev.horario);

  String corpo;
  serializeJson(doc, corpo);

  int code = chamarFirebase(
    clienteFirestore,
    "PATCH",
    docUrl() + "?updateMask.fieldPaths=eventoId&updateMask.fieldPaths=eventoTipo"
               "&updateMask.fieldPaths=eventoSlot&updateMask.fieldPaths=eventoHorario",
    corpo);

  if (code == 401 || code == 403) idToken = "";
  return code == 200;
}

bool enviarEstoque() {
  String estoqueTexto;
  {
    Trava t;
    JsonDocument lista;
    JsonArray arr = lista.to<JsonArray>();
    for (int i = 0; i < NUM_SLOTS; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["s"] = i + 1;
      o["nome"] = nomeConhecido[i];
      o["qtd"] = quantidadeAtual[i];
      o["baixo"] = estoqueBaixo(i);
      o["consumo"] = consumoPorDose[i];
      o["doses"]   = dosesRestantes(i);
    }
    serializeJson(lista, estoqueTexto);
  }

  JsonDocument doc;
  doc["fields"]["estoqueAtual"]["stringValue"] = estoqueTexto;
  String corpo;
  serializeJson(doc, corpo);

  return chamarFirebase(clienteFirestore, "PATCH",
    docUrl() + "?updateMask.fieldPaths=estoqueAtual", corpo) == 200;
}

// Os campos do Firestore podem vir como integerValue (texto), stringValue ou doubleValue
int campoNum(JsonVariantConst f) {
  JsonVariantConst v = f["integerValue"];
  if (v.isNull()) v = f["stringValue"];
  if (v.isNull()) v = f["doubleValue"];
  if (v.isNull()) return 0;
  if (v.is<const char*>()) return atoi(v.as<const char*>());
  return v.as<int>();
}

String campoTexto(JsonVariantConst f) {
  const char* s = f["stringValue"] | "";
  if (s[0] == 0) s = f["integerValue"] | "";
  return String(s);
}

// Uma unica consulta traz agenda + comandos do app
void lerFirebase() {
  String url = docUrl() +
    "?mask.fieldPaths=agendaJson&mask.fieldPaths=agendaAt"
    "&mask.fieldPaths=cmdAt&mask.fieldPaths=cmdTipo&mask.fieldPaths=cmdSlot&mask.fieldPaths=cmdMin"
    "&mask.fieldPaths=cfgSom&mask.fieldPaths=cfgLuz&mask.fieldPaths=estoqueApp";
  int code = chamarFirebase(clienteFirestore, "GET", url, "");
  if (code == 401 || code == 403) { idToken = ""; return; }
  if (code != 200) { Serial.printf("Ler Firebase: HTTP %d\n", code); return; }

  JsonDocument doc;
  if (deserializeJson(doc, resposta)) return;
  JsonVariantConst campos = doc["fields"];

  // ---- configuração de alertas (som / luz) ----
  JsonVariantConst vSom = campos["cfgSom"]["booleanValue"];
  if (!vSom.isNull()) cfgSom = vSom.as<bool>();
  JsonVariantConst vLuz = campos["cfgLuz"]["booleanValue"];
  if (!vLuz.isNull()) cfgLuz = vLuz.as<bool>();

  // ---- comandos do app (confirmar / adiar) ----
  String cid = campoTexto(campos["cmdAt"]);
  if (!cmdInicializado) {
    // primeira leitura depois de ligar: so memoriza, nao executa comando antigo
    ultimoCmd = cid;
    cmdInicializado = true;
  } else if (cid != "" && cid != ultimoCmd) {
    ultimoCmd = cid;
    String tipo = campoTexto(campos["cmdTipo"]);
    tipo.toLowerCase();

    Comando c;
    c.tipo = CMD_NENHUM;
    if (tipo == "confirmar" || tipo == "confirmado" || tipo == "tomado" || tipo == "confirm")
      c.tipo = CMD_CONFIRMAR;
    else if (tipo == "adiar" || tipo == "adiado" || tipo == "snooze" || tipo == "soneca")
      c.tipo = CMD_ADIAR;
    else if (tipo == "perdido" || tipo == "perdida" || tipo == "missed" || tipo == "skip")
  c.tipo = CMD_PERDIDO;

    c.slot = (int8_t)campoNum(campos["cmdSlot"]);   // 1..6 (0 = o alerta mostrado na tela)
    int mi = campoNum(campos["cmdMin"]);
    c.minutos = (mi > 0) ? mi : ADIAR_PADRAO_MIN;

    Serial.printf("Comando do app: %s slot %d (%d min)\n", tipo.c_str(), (int)c.slot, (int)c.minutos);
    if (c.tipo != CMD_NENHUM) xQueueSend(filaCmd, &c, 0);
    statusUrgente = true;   // avisa o app que recebemos (cmdAck)
  }

  // ---- ajustes manuais de estoque (app) ----
  // formato de cada slot: estoqueApp.s1 .. s6 = "versao:quantidade"
  JsonVariantConst mapaEstq = campos["estoqueApp"]["mapValue"]["fields"];
  if (!mapaEstq.isNull()) {
    for (int i = 0; i < NUM_SLOTS; i++) {
      char chave[3] = { 's', (char)('1' + i), 0 };
      const char* txt = mapaEstq[chave]["stringValue"] | "";
      if (!txt[0]) continue;

      char* sep = nullptr;
      uint32_t ver = strtoul(txt, &sep, 10);
      if (!sep || *sep != ':' || ver == 0) continue;
      if (ver == estqVer[i]) continue;                  // ja aplicado

      long q = atol(sep + 1);
      if (q < 0) q = 0;
      if (q > 65535) q = 65535;

      CmdEstoque ce{ (uint8_t)i, (uint16_t)q, ver };
      xQueueSend(filaEstoque, &ce, 0);
      statusUrgente = true;
    }
  }

  // ---- agenda ----
  String novoAt = campos["agendaAt"]["stringValue"] | "";
  String json   = campos["agendaJson"]["stringValue"] | "";

  String atual;
  {
    Trava t;
    atual = agendaAt;
  }

  if (json.length() == 0) {
    // Sem agenda na nuvem e nunca houve agenda aqui: nada a fazer.
    if (novoAt == "" && atual == "") return;
    // Agenda esvaziada pelo app (ou campo apagado): esvazia a da caixa tambem,
    // senao ficariam compartimentos "fantasmas" (ex.: estoque baixo de um slot removido).
    json = "[]";
  }
  if (novoAt == atual) return;  // nada novo

  JsonDocument nova;
  if (deserializeJson(nova, json)) {
    Serial.println("Agenda recebida e invalida.");
    return;
  }
  {
    Trava t;
    agenda = nova;
    agendaAt = novoAt;
  }
  agendaNovaFlag = true;     // o loop salva, atualiza estoque e revalida alertas
  statusUrgente = true;
  Serial.println("Agenda atualizada pelo app.");
}

void tarefaRede(void *param) {
  unsigned long ultimoPoll = 0, ultimoStatus = 0, ultimaTentativaHora = 0;
  unsigned long ultimaReconexao = millis();
  EventoDose evPend;
  bool temEvPend = false;

  for (;;) {
    // standby: WiFi desligado
    if (!sistemaLigado) {
      if (WiFi.getMode() != WIFI_OFF) {
        if (WiFi.status() == WL_CONNECTED && idToken != "") enviarStatus(true); // "standby"
        clienteFirestore.stop();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // acabou de ligar
    if (WiFi.getMode() == WIFI_OFF) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_SENHA);
      ultimaReconexao = millis();
      ultimoPoll = 0;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - ultimaReconexao > 8000) {
        ultimaReconexao = millis();
        WiFi.reconnect();
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!horaSincronizada && (ultimaTentativaHora == 0 || millis() - ultimaTentativaHora > 60000)) {
      ultimaTentativaHora = millis();
      sincronizarHoraRede();
    }

    if (idToken == "" || (millis() - tokenMs > (50UL * 60 * 1000))) {
      if (!fazerLogin()) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
    }

    // 1) eventos da caixa -> Firebase (prioridade, com retry)
    int enviados = 0;
    while (enviados < 4 && (temEvPend || xQueueReceive(filaDoses, &evPend, 0) == pdTRUE)) {
      temEvPend = true;
      if (!enviarEvento(evPend)) {
        Serial.println("Falha ao enviar evento (tenta de novo no proximo ciclo).");
        break;                  // mantem evPend guardado
      }
      temEvPend = false;
      if (evPend.tipo == EV_TOMADO) estoqueMudou = true;
      enviados++;
      vTaskDelay(pdMS_TO_TICKS(300));   // folga para o app enxergar cada evento
    }

    // 2) estoque
    if (estoqueMudou) {
      estoqueMudou = false;
      if (!enviarEstoque()) estoqueMudou = true;
    }

    // 3) consulta agenda/comandos (rapido durante alerta)
    unsigned long intervalo = existeAlerta ? INTERVALO_ALERTA_MS : INTERVALO_NORMAL_MS;
    if (ultimoPoll == 0 || millis() - ultimoPoll >= intervalo) {
      ultimoPoll = millis();
      lerFirebase();
    }

    // 4) status
    if (statusUrgente || ultimoStatus == 0 || millis() - ultimoStatus >= INTERVALO_STATUS_MS) {
      statusUrgente = false;
      ultimoStatus = millis();
      enviarStatus();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ===================== LCD =====================

void escreverLinha(int linha, String texto) {
  if (texto.length() > 20) texto = texto.substring(0, 20);
  while (texto.length() < 20) texto += ' ';
  lcd.setCursor(0, linha);
  lcd.print(texto);
}

void mostrarOcioso() {
  DateTime agora = horaAtual();
  char texto[24];

  escreverLinha(0, "CHRONOBOX");
  snprintf(texto, sizeof(texto), "Hora: %02d:%02d:%02d", agora.hour(), agora.minute(), agora.second());
  escreverLinha(1, texto);
  snprintf(texto, sizeof(texto), "Data: %02d/%02d/%04d", agora.day(), agora.month(), agora.year());
  escreverLinha(2, texto);

  // compartimentos com estoque baixo (so os que ainda existem na agenda)
  int baixos[NUM_SLOTS];
  int nBaixos = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (estoqueBaixo(i)) baixos[nBaixos++] = i;
  }

  // Linha 3 alterna a cada 3 s: conexao -> "ESTOQUE BAIXO" -> "Compartimento N"
  int seg  = agora.second();
  int fase = (seg / 3) % 3;

  if (todosAbertos) {
    escreverLinha(3, "TODOS ABERTOS");
  } else if (nBaixos > 0 && fase == 1) {
    escreverLinha(3, "ESTOQUE BAIXO");
  } else if (nBaixos > 0 && fase == 2) {
    int k = baixos[(seg / 9) % nBaixos];
    snprintf(texto, sizeof(texto), "Compartimento %d", k + 1);
    escreverLinha(3, texto);
  } else {
    escreverLinha(3, WiFi.status() == WL_CONNECTED ? "Conectado" : "Sem WiFi");
  }
}

void mostrarAlertaNaTela() {
  int i = slotMostrado;
  if (i < 0) return;

  int total = 0, posicao = 0;
  for (int k = 0; k < NUM_SLOTS; k++) {
    if (alertaAtivo[k]) {
      total++;
      if (k <= i) posicao = total;
    }
  }

  char texto[24];
  escreverLinha(0, alertaNome[i]);
  snprintf(texto, sizeof(texto), "Compartimento: %d", i + 1);
  escreverLinha(1, texto);
  snprintf(texto, sizeof(texto), "Dose:%d Restam:%d", alertaDose[i], dosesRestantes(i));
  escreverLinha(2, texto);
  snprintf(texto, sizeof(texto), "Alerta %d/%d", posicao, total);
  escreverLinha(3, texto);
}

// ===================== ALERTAS =====================

int contarAlertas() {
  int total = 0;
  for (int i = 0; i < NUM_SLOTS; i++) if (alertaAtivo[i]) total++;
  return total;
}

// proximo slot com alerta ativo depois de "depois" (circular); -1 se nao houver
int proximoAlerta(int depois) {
  for (int k = 1; k <= NUM_SLOTS; k++) {
    int idx = (depois + k + NUM_SLOTS) % NUM_SLOTS;
    if (alertaAtivo[idx]) return idx;
  }
  return -1;
}

void iniciarAlerta(int i, String nome, int dose, bool usaLed, bool usaBuzzer, bool daAgenda, int hItem, int mItem) {
  alertaAtivo[i]  = true;
  alertaNome[i]   = nome;
  alertaDose[i]   = dose;
  alertaLed[i]    = usaLed;
  alertaBuzzer[i] = usaBuzzer;
  alertaH[i] = daAgenda ? hItem : -1;
  alertaM[i] = daAgenda ? mItem : -1;
  fechaEm[i] = millis() + TEMPO_ABERTO_MS;
  adiaUnix[i] = 0;
  fechaPosConfirmarEm[i] = 0;

  moverServo(i, ANGULO_ABERTO, 0);

  if (slotMostrado == -1) {
    slotMostrado = i;
    ultimaTroca = millis();
  }
  ultimoPisca = 0;  // redesenha o LCD na hora
  Serial.printf("Hora do remedio! Slot %d (%s) aberto.\n", i + 1, nome.c_str());
}

void fecharSlot(int i) {
  moverServo(i, ANGULO_FECHADO, 0);
  digitalWrite(PINOS_LED[i], LOW);
  alertaAtivo[i] = false;
  fechaEm[i] = 0;
  fechaPosConfirmarEm[i] = 0;
}

void fecharConfirmados() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (fechaPosConfirmarEm[i] != 0 && (long)(millis() - fechaPosConfirmarEm[i]) >= 0) {
      fechaPosConfirmarEm[i] = 0;
      moverServo(i, ANGULO_FECHADO, 0);
      Serial.printf("Slot %d fechado apos confirmacao.\n", i + 1);
    }
  }
}

// encerra o alerta (LED/buzzer/tela) mas NÃO mexe no servo
void pararAlerta(int i) {
  digitalWrite(PINOS_LED[i], LOW);
  alertaAtivo[i] = false;
  fechaEm[i] = 0;
}

void depoisDeFechar(int i) {
  if (slotMostrado == i) {
    slotMostrado = proximoAlerta(i);
    ultimaTroca = millis();
    ultimoPisca = 0;
  }
  if (contarAlertas() == 0) {
    desligarBuzzer();
    slotMostrado = -1;
    ultimoSegundoTela = -1;  // forca redesenhar a tela ociosa
    lcd.clear();
  }
}

// variáveis globais
bool salvarEstoquePendente = false;
unsigned long salvarEstoqueEm = 0;

void registrarDose(int i) {
  int consumo = (consumoPorDose[i] > 0) ? consumoPorDose[i] : 1;
  {
    Trava t;
    quantidadeAtual[i] -= consumo;
    if (quantidadeAtual[i] < 0) quantidadeAtual[i] = 0;
  }
  salvarEstoquePendente = true;                 // grava depois
  salvarEstoqueEm = millis() + TEMPO_MOVER_MS + 300;

  emitirEvento(i, EV_TOMADO);
  estoqueMudou = true;
}

// confirma a dose de um slot (pelo botao da caixa OU pelo app)
void confirmarSlot(int i) {
  bool estavaAtivo = alertaAtivo[i];
  if (estavaAtivo) pararAlerta(i);        // para som/luz, tampa continua aberta
  adiaUnix[i] = 0;

  Serial.printf("Dose confirmada: slot %d (%s)\n", i + 1, alertaNome[i].c_str());
  registrarDose(i);

  if (estavaAtivo) {
    fechaPosConfirmarEm[i] = millis() + TEMPO_FECHAR_POS_CONFIRMAR_MS;
    depoisDeFechar(i);
  }
}

void confirmarDose() {
  int i = slotMostrado;
  if (i < 0 || !alertaAtivo[i]) return;
  confirmarSlot(i);
}

// adia: fecha a tampa agora e toca de novo daqui a "minutos" (controlado pela propria caixa)
void adiarSlot(int i, int minutos) {
  if (minutos <= 0) minutos = ADIAR_PADRAO_MIN;
  if (!alertaAtivo[i] && adiaUnix[i] == 0 && alertaNome[i] == "") return;  // nunca tocou: nada a adiar

  bool estavaAtivo = alertaAtivo[i];
  if (estavaAtivo) { fecharSlot(i); atualizarServos(); }
  adiaUnix[i] = (uint32_t)unixLocal + (uint32_t)minutos * 60UL;
  Serial.printf("Slot %d adiado por %d min.\n", i + 1, minutos);

  emitirEvento(i, EV_ADIADO);

  if (estavaAtivo) depoisDeFechar(i);
}

// App marcou "nao administrada": para som/luz, fecha a tampa e cancela soneca.
// NAO emite evento: o app ja registrou no relatorio.
void perderSlot(int i) {
  bool estavaAtivo = alertaAtivo[i];
  adiaUnix[i] = 0;
  if (estavaAtivo) {
    fecharSlot(i);
    depoisDeFechar(i);
  }
  Serial.printf("Slot %d marcado como nao administrado pelo app.\n", i + 1);
}

void processarComandos() {
  Comando c;
  while (xQueueReceive(filaCmd, &c, 0) == pdTRUE) {
    int i = (int)c.slot - 1;
    if (i < 0 || i >= NUM_SLOTS) i = slotMostrado;   // app nao mandou o slot: usa o da tela
    if (i < 0) continue;

    if (c.tipo == CMD_CONFIRMAR) {
      if (alertaAtivo[i] || adiaUnix[i] != 0) confirmarSlot(i);
    } else if (c.tipo == CMD_ADIAR) {
      adiarSlot(i, c.minutos);
    } else if (c.tipo == CMD_PERDIDO) {
      if (alertaAtivo[i] || adiaUnix[i] != 0) perderSlot(i);
    }
  }
}

// Ajuste manual feito na tela de estoque do app (+/-)
// (estava declarada DENTRO do loop(), o que nao compila em C++)
void processarEstoqueApp() {
  CmdEstoque ce;
  bool mudou = false;

  while (xQueueReceive(filaEstoque, &ce, 0) == pdTRUE) {
    int i = ce.slot;
    if (i < 0 || i >= NUM_SLOTS) continue;
    if (ce.ver == estqVer[i]) continue;      // ja aplicado

    estqVer[i] = ce.ver;
    mudou = true;

    {
      Trava t;
      if (nomeConhecido[i] != "") {          // slot vazio: a agenda traz a quantidade depois
        quantidadeAtual[i] = ce.qtd;
        Serial.printf("App ajustou o estoque do slot %d: %d unidades (%d doses).\n",
                      i + 1, quantidadeAtual[i], dosesRestantes(i));
      }
    }
  }

  if (mudou) {
    salvarEstoqueLocal();
    estoqueMudou = true;        // devolve o estoque atualizado ao Firebase
    ultimoSegundoTela = -1;     // redesenha a tela
    ultimoPisca = 0;
  }
}

void verificarAdiados() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (adiaUnix[i] != 0 && !alertaAtivo[i] && (uint32_t)unixLocal >= adiaUnix[i]) {
      adiaUnix[i] = 0;
      iniciarAlerta(i, alertaNome[i], alertaDose[i], alertaLed[i], alertaBuzzer[i], false);
    }
  }
}

void fecharAlertasExpirados() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (alertaAtivo[i] && millis() > fechaEm[i]) {
      Serial.printf("Alerta expirado sem confirmacao: slot %d (%s)\n", i + 1, alertaNome[i].c_str());
      emitirEvento(i, EV_PERDIDO);
      fecharSlot(i);
      depoisDeFechar(i);
    }
  }
}

// chamar com o mutex travado
bool itemExiste(int i, int h, int m) {
  for (JsonObject item : agenda.as<JsonArray>()) {
    if ((item["s"] | 0) == i + 1 && (item["h"] | -1) == h && (item["m"] | -1) == m) return true;
  }
  return false;
}

// Agenda nova chegou do app (roda no loop)
void processarAgendaNova() {
  if (!agendaNovaFlag) return;
  agendaNovaFlag = false;
  ultimoMinuto = -1; // verificarHorarios roda ja neste ciclo

  Trava t;
  salvarAgendaLocal();
  sincronizarEstoque();

  for (int i = 0; i < NUM_SLOTS; i++) {
    bool removido = !slotNaAgenda[i];   // medicamento nao esta mais na agenda

    if (removido) {
      adiaUnix[i] = 0;        // cancela soneca pendente
      alertaNome[i] = "";
    }

    // Removeu o medicamento, ou o app removeu/moveu o horario que esta tocando
    if (alertaAtivo[i] && (removido || (alertaH[i] >= 0 && !itemExiste(i, alertaH[i], alertaM[i])))) {
      Serial.printf("App alterou/removeu o slot %d: alerta encerrado.\n", i + 1);
      fecharSlot(i);
      depoisDeFechar(i);
    }
  }
}

void verificarHorarios() {
  DateTime agora = horaAtual();
  if (agora.minute() == ultimoMinuto) return;
  ultimoMinuto = agora.minute();

  int agoraMin = agora.hour() * 60 + agora.minute();
  uint32_t minutoEpoch = (uint32_t)unixLocal / 60;

  Trava t;
  for (JsonObject item : agenda.as<JsonArray>()) {
    int h = item["h"] | -1;
    int m = item["m"] | -1;
    if (h < 0 || m < 0) continue;

    int antec = item["a"] | 0;                 // minutos de antecedência
    if (antec < 0) antec = 0;
    if (antec > 60) antec = 60;

    // minutos que faltam para a próxima ocorrência da dose (0..1439)
    int faltam = (h * 60 + m - agoraMin + 1440) % 1440;
    if (faltam > antec) continue;              // fora da janela [dose-antec, dose]

    // dia da semana da DOSE (se a antecedência cruza a meia-noite, é amanhã)
    int diaDose = agora.dayOfTheWeek();
    if (agoraMin + faltam >= 1440) diaDose = (diaDose + 1) % 7;

    if (item["w"].is<JsonArray>()) {
      bool ok = false;
      for (JsonVariant d : item["w"].as<JsonArray>()) {
        if (d.as<int>() == diaDose) ok = true;
      }
      if (!ok) continue;
    }

    int slot = item["s"] | 0;
    if (slot < 1 || slot > NUM_SLOTS) continue;

    // cada dose dispara uma única vez
    uint32_t chave = (minutoEpoch + faltam) * 8 + (slot - 1);
    if (jaDisparou(chave)) continue;
    marcarDisparo(chave);

    if (alertaAtivo[slot - 1]) continue;

    String nome = item["n"] | "";
    if (nome == "") nome = "Compartimento " + String(slot);

    int dose = item["d"] | 1;
    bool usaLed = item["led"] | true;
    bool usaBuzzer = item["buzzer"] | false;

    iniciarAlerta(slot - 1, nome, dose, usaLed, usaBuzzer, true, h, m);
  }
}

void atualizarAlertasNaTela() {
  // com 2+ alertas, alterna a tela a cada 5 s
  if (contarAlertas() > 1 && (millis() - ultimaTroca >= 5000)) {
    ultimaTroca = millis();
    slotMostrado = proximoAlerta(slotMostrado);
    ultimoPisca = 0;
  }

  if (millis() - ultimoPisca >= 500) {
    ultimoPisca = millis();
    estadoPisca = !estadoPisca;

    bool algumBuzzer = false;
    for (int i = 0; i < NUM_SLOTS; i++) {
      if (!alertaAtivo[i]) continue;
      digitalWrite(PINOS_LED[i], cfgLuz ? estadoPisca : LOW);
      if (cfgSom) algumBuzzer = true;
    }
    if (algumBuzzer && estadoPisca) ligarBuzzer();
    else desligarBuzzer();

    mostrarAlertaNaTela();
  }
}

// ===================== ABRIR / FECHAR TUDO =====================

void abrirTodos() {
  for (int i = 0; i < NUM_SLOTS; i++) moverServo(i, ANGULO_ABERTO, i * ESCALONAR_MS);
  todosAbertos = true;
  lcd.clear();
  escreverLinha(1, "Todos os compart.");
  escreverLinha(2, "estao abertos");
  mensagemAte = millis() + 3000;
}

void fecharTodos() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    moverServo(i, ANGULO_FECHADO, i * ESCALONAR_MS);
    digitalWrite(PINOS_LED[i], LOW);
  }
  todosAbertos = false;
  lcd.clear();
  escreverLinha(1, "Fechando todos os");
  escreverLinha(2, "compartimentos");
  mensagemAte = millis() + 3000;
}

// ===================== BOTOES =====================

void verificarBotaoConfirmar() {
  static bool estavel = false;        // estado ja filtrado (true = apertado)
  static bool ultimaLeitura = false;
  static unsigned long mudouEm = 0;

  bool leitura = lerBotao(PINO_BOTAO_CONFIRMAR);
  if (leitura != ultimaLeitura) {
    ultimaLeitura = leitura;
    mudouEm = millis();
  }

  if (leitura != estavel && millis() - mudouEm >= DEBOUNCE_MS) {
    estavel = leitura;

    if (estavel) {
      // ---- apertou ----
      if (contarAlertas() > 0) {          // alerta ativo = confirmar dose
        confirmarDose();
        cliques = 0;
      } else {
        if (millis() - ultimoClique > JANELA_CLIQUES) cliques = 0;
        cliques++;
        ultimoClique = millis();
        Serial.printf("CLIQUE %d\n", cliques);

        if (cliques == 4) {               // 4o clique: precisa segurar
          inicioSegurar = millis();
          segurou = false;
          Serial.println("SEGURE O BOTAO...");
        }
      }
    } else {
      // ---- soltou ----
      if (cliques >= 4 && !segurou) {     // soltou antes de completar: recomeca o combo
        cliques = 0;
        Serial.println("Combo cancelado (soltou cedo).");
      }
    }
  }

  // 4o clique segurado o tempo todo
  if (cliques == 4 && estavel && !segurou && millis() - inicioSegurar >= TEMPO_SEGURAR) {
    segurou = true;
    cliques = 0;
    Serial.println("COMANDO DE EMERGENCIA");
    if (todosAbertos) fecharTodos();
    else abrirTodos();
  }

  // demorou demais entre os cliques
  if (!estavel && cliques > 0 && millis() - ultimoClique > JANELA_CLIQUES) {
    cliques = 0;
  }
}

// Liga/desliga (o WiFi e controlado pela tarefa de rede)
void alternarPower() {
  sistemaLigado = !sistemaLigado;

  if (sistemaLigado) {
    Serial.println("Caixa LIGADA");
    ultimoMinuto = -1;
    ultimoSegundoTela = -1;
    lcd.backlight();
    lcd.clear();
    escreverLinha(1, "      Ligando...");
    delay(800);
    mostrarOcioso();
  } else {
    Serial.println("Caixa DESLIGADA (standby)");
    lcd.clear();
    escreverLinha(1, "    Desligando...");
    desligarBuzzer();
    for (int i = 0; i < NUM_SLOTS; i++) {
      digitalWrite(PINOS_LED[i], LOW);
      moverServo(i, ANGULO_FECHADO, i * ESCALONAR_MS);
      alertaAtivo[i] = false;
      alertaLed[i] = false;
      alertaBuzzer[i] = false;
      fechaEm[i] = 0;
      adiaUnix[i] = 0;
      fechaPosConfirmarEm[i] = 0;
    }
    slotMostrado = -1;
    todosAbertos = false;
    existeAlerta = false;
    cliques = 0;
    delay(600);
    lcd.clear();
    lcd.noBacklight();
  }
}

// Segurar o botao power por TEMPO_POWER_MS liga/desliga (com debounce)
void verificarBotaoPower() {
  static bool estavel = false;          // estado ja filtrado (true = apertado)
  static bool ultimaLeitura = false;
  static unsigned long mudouEm = 0;
  static unsigned long inicio = 0;
  static bool tratado = false;

  bool leitura = lerBotao(PINO_BOTAO_POWER);
  if (leitura != ultimaLeitura) {
    ultimaLeitura = leitura;
    mudouEm = millis();
  }

  // so aceita a mudanca depois de estabilizar (debounce)
  if (leitura != estavel && millis() - mudouEm >= DEBOUNCE_MS) {
    estavel = leitura;
    if (estavel) {
      inicio = millis();
      tratado = false;
      Serial.println("POWER: apertado (segure ate ligar/desligar)");
    } else {
      Serial.println("POWER: solto");
    }
  }

  // segurou o tempo necessario: age uma unica vez por aperto
  if (estavel && !tratado && millis() - inicio >= TEMPO_POWER_MS) {
    tratado = true;
    Serial.println("POWER: alternando estado");
    alternarPower();
  }
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== CHRONOBOX iniciando ===");
  esp_reset_reason_t motivo = esp_reset_reason();
  Serial.printf("Motivo do ultimo reset: %d%s\n", (int)motivo,
                motivo == ESP_RST_BROWNOUT ? "  <-- BROWNOUT (queda de tensao: alimentacao fraca)" : "");

  mtx = xSemaphoreCreateMutex();
  filaDoses = xQueueCreate(16, sizeof(EventoDose));
  filaCmd = xQueueCreate(4, sizeof(Comando));
  filaEstoque = xQueueCreate(8, sizeof(CmdEstoque));
  clienteFirestore.setInsecure();

  pinMode(PINO_BUZZER, OUTPUT);
  desligarBuzzer();  // low level trigger: HIGH = silencio

  pinMode(PINO_BOTAO_POWER,     BOTAO_ATIVO_ALTO ? INPUT_PULLDOWN : INPUT_PULLUP);
  pinMode(PINO_BOTAO_CONFIRMAR, BOTAO_ATIVO_ALTO ? INPUT_PULLDOWN : INPUT_PULLUP);

  if (!LittleFS.begin(true)) Serial.println("Falha ao iniciar o LittleFS!");

  Wire.begin();  // SDA = 21, SCL = 22
  if (!rtc.begin()) {
    Serial.println("RTC nao encontrado!");
  } else if (rtc.lostPower()) {
    Serial.println("RTC sem hora valida, usando a hora da compilacao.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  unixLocal = rtc.now().unixtime();

  lcd.init();
  lcd.backlight();
  escreverLinha(0, "   CHRONOBOX");
  escreverLinha(1, "Iniciando...");

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  for (int i = 0; i < NUM_SLOTS; i++) {
    estoqueMinimo[i] = ESTOQUE_MINIMO_PADRAO;
    consumoPorDose[i] = 1;
    alertaH[i] = -1;
    alertaM[i] = -1;
    pinMode(PINOS_LED[i], OUTPUT);
    digitalWrite(PINOS_LED[i], LOW);
  }

  // Fecha os servos UM POR VEZ (escalonado): mover os 6 juntos derruba a tensao e reinicia o ESP32
  for (int i = 0; i < NUM_SLOTS; i++) moverServo(i, ANGULO_FECHADO, i * ESCALONAR_MS);
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)NUM_SLOTS * ESCALONAR_MS + TEMPO_MOVER_MS + 200) {
    atualizarServos();
    delay(10);
  }

  bool temAgendaLocal = carregarAgendaLocal();
  carregarEstoqueLocal();

  // Reconcilia o estoque salvo com a agenda salva. Sem isso, um compartimento
  // removido que ficou gravado na flash (estoque.json) nunca era limpo se a agenda
  // da nuvem ja fosse igual a local, e o "Estoque baixo" ficava aparecendo.
  if (temAgendaLocal) {
    Trava t;
    sincronizarEstoque(false);   // false: nao reaplicar "resetQtd" antigo
  }
  estoqueMudou = true;  // envia o estoque ao Firebase quando conectar

  WiFi.mode(WIFI_STA);
  delay(300);
  boxId = WiFi.macAddress();
  boxId.replace(":", "");
  boxId.toUpperCase();
  Serial.println("CODIGO DA CAIXA (digite no app): " + boxId);
  escreverLinha(2, "Cod: " + boxId);

  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  // a rede roda em outra tarefa (nucleo 0) e nao trava mais o loop
  xTaskCreatePinnedToCore(tarefaRede, "rede", 16384, NULL, 1, NULL, 0);

  delay(1200);
  lcd.clear();
  mostrarOcioso();
  Serial.println("Setup concluido.");
}

void loop() {
  verificarBotaoPower();
  atualizarServos();

  if (!sistemaLigado) {  // standby: so escuta o botao power
    delay(30);
    return;
  }

  atualizarRelogio();

  if (ajustarRtcFlag) {
    ajustarRtcFlag = false;
    rtc.adjust(DateTime((uint32_t)ntpLocalUnix));
    unixLocal = ntpLocalUnix;
  }

  processarAgendaNova();
  processarComandos();
  processarEstoqueApp();
  verificarAdiados();
  verificarHorarios();
  fecharAlertasExpirados();
  fecharConfirmados();
  verificarBotaoConfirmar();

  if (salvarEstoquePendente && (long)(millis() - salvarEstoqueEm) >= 0) {
  salvarEstoquePendente = false;
  salvarEstoqueLocal();
}

  existeAlerta = (contarAlertas() > 0);

  if (existeAlerta) {
    atualizarAlertasNaTela();
  } else if (millis() > mensagemAte) {
    DateTime agora = horaAtual();
    if ((int)agora.second() != ultimoSegundoTela) {
      ultimoSegundoTela = agora.second();
      mostrarOcioso();
    }
  }

  delay(10);
}
