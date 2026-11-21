/*
  comandos.cpp - Comandos do Monitor Serial, "?" e log CSV.

  Monitor Serial: 115200 baud, final de linha "Nova linha".
  Aceita virgula ou ponto nos numeros ("d 0,3" = "d 0.3").

  LOG CSV ("log 1000"): uma linha de cabecalho e depois uma linha a cada
  intervalo, separada por ponto e virgula, com ponto decimal:
    tempo_ms;modo;D;alfa;Vin;Iin;Vo;Io;Pin;Pout;eta;Vo_med;Vo_rms;Io_med;Io_rms;f_rede
  Os valores sao as medias da ultima janela de 1 s. Campo vazio = nao se
  aplica ao modo (INA226 so nos conversores, ADS1115 so nos retificadores) ou
  o sensor nao responde. eta = Pout/Pin (%) nos modos BOOST
  e BUCK e Pdc/Pac (%) nos modos de retificador. Mensagens que aparecem
  durante o log comecam com "# ".

  PAINEL WEB: a caixa "Comando (igual a serial)" da pagina
  manda qualquer comando daqui, com as mesmas regras (web.cpp decide quem pode
  mandar o que). A resposta e o mesmo texto da serial, capturado pela classe Eco.

  ESPELHO DA SERIAL: todo texto passa pela classe Tee, que
  escreve na Serial e copia num buffer circular (o "espelho"). A pagina mostra o
  espelho como um Monitor Serial (rota /serial). O texto na serial nao muda. Os
  comandos digitados na serial aparecem so no espelho, como "[serial] <comando>",
  para a pagina mostrar o que foi pedido (o Monitor Serial da IDE nao mostra).
*/
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "comandos.h"
#include "controle.h"
#include "pwm_conversores.h"
#include "disparo_scr.h"
#include "medicao.h"
#include "painel.h"
#include "tela.h"
#include "registro.h"
#include "web.h"

// ---- espelho da serial: buffer circular com tudo o que foi escrito na serial ----
static char             *espelho      = nullptr;
static size_t            espelhoCap   = 0;      // potencia de 2
static volatile uint32_t espelhoTotal = 0;      // bytes escritos desde o reset (o byte k fica em k & (cap - 1))
static SemaphoreHandle_t espelhoTrava = nullptr;

void espelhoIniciar() {
  if (espelho != nullptr) return;
  espelhoTrava = xSemaphoreCreateMutex();
  if (psramFound()) {
    espelho = (char *)ps_malloc(ESPELHO_BYTES);
    espelhoCap = ESPELHO_BYTES;
  }
  if (espelho == nullptr) {   // sem PSRAM: um espelho pequeno na RAM interna
    espelho = (char *)malloc(ESPELHO_BYTES_SEM_PSRAM);
    espelhoCap = ESPELHO_BYTES_SEM_PSRAM;
  }
  if (espelho == nullptr || espelhoTrava == nullptr) { espelho = nullptr; espelhoCap = 0; }
}

// Copia n bytes para o espelho. Custo: um mutex (que nao bloqueia interrupcoes) e um
// memcpy. Nunca de interrupcao: ali nao se escreve na serial (e o mutex nao pode).
static void espelhoGuardar(const uint8_t *b, size_t n) {
  if (espelho == nullptr || n == 0 || xPortInIsrContext()) return;
  xSemaphoreTake(espelhoTrava, portMAX_DELAY);
  uint32_t total = espelhoTotal;
  if (n > espelhoCap) {          // mais que o buffer inteiro: fica so o fim
    total += (uint32_t)(n - espelhoCap);
    b += n - espelhoCap;
    n = espelhoCap;
  }
  size_t pos = total & (espelhoCap - 1);
  size_t a = espelhoCap - pos;
  if (a > n) a = n;
  memcpy(espelho + pos, b, a);
  if (n > a) memcpy(espelho, b + a, n - a);
  espelhoTotal = total + (uint32_t)n;
  xSemaphoreGive(espelhoTrava);
}

size_t espelhoLer(uint32_t desde, char *dst, size_t max, uint32_t &ini, uint32_t &total, bool &perdeu, bool &reinicio) {
  ini = 0; total = 0; perdeu = false; reinicio = false;
  if (espelho == nullptr) return 0;
  xSemaphoreTake(espelhoTrava, portMAX_DELAY);
  total = espelhoTotal;
  uint32_t antigo = (total > espelhoCap) ? total - (uint32_t)espelhoCap : 0;   // o mais antigo que ainda existe
  uint32_t i = desde;
  if (i > total) { reinicio = true; i = 0; }   // a pagina conhecia um contador maior: a placa reiniciou
  if (i < antigo) { perdeu = true; i = antigo; }   // esse trecho ja foi sobrescrito
  size_t n = total - i;
  if (n > max) n = max;
  size_t pos = i & (espelhoCap - 1);
  size_t a = espelhoCap - pos;
  if (a > n) a = n;
  memcpy(dst, espelho + pos, a);
  if (n > a) memcpy(dst + a, espelho, n - a);
  xSemaphoreGive(espelhoTrava);
  ini = i;
  return n;
}

// Saida unica: Serial + espelho.
class Tee : public Print {
 public:
  size_t write(uint8_t c) override {
    espelhoGuardar(&c, 1);
    return Serial.write(c);
  }
  size_t write(const uint8_t *b, size_t len) override {
    espelhoGuardar(b, len);
    return Serial.write(b, len);
  }
};
static Tee tee;
Print &saidaSerial() { return tee; }

// ---- saida dos comandos: a saida unica e, durante um comando do painel web, uma copia para a resposta ----
static const size_t ECO_MAX = 4096;   // resposta de um comando da pagina (o "?" dos retificadores tem ~2,2 kB)
static const char ECO_CORTE[] = "\n(resposta cortada; veja a serial)";
class Eco : public Print {
 public:
  size_t write(uint8_t c) override {
    guardar(c);
    return tee.write(c);
  }
  size_t write(const uint8_t *b, size_t len) override {
    for (size_t i = 0; i < len; i++) guardar(b[i]);
    return tee.write(b, len);
  }
  bool capturando = false;
  bool cortado = false;
  char buf[ECO_MAX];
  size_t n = 0;
 private:
  void guardar(uint8_t c) {   // deixa lugar para o aviso de corte e o '\0'
    if (!capturando) return;
    if (n < sizeof(buf) - sizeof(ECO_CORTE)) buf[n++] = (char)c;
    else cortado = true;
  }
};
static Eco eco;
Print &con() { return eco; }

static uint32_t logMs       = 0;  // 0 = log desligado
static uint32_t ultimoLogMs = 0;
#if !PAINEL_MANUAL
static uint32_t  ligaPedidoMs = 0; // sem pots: "l" esperando o segundo "l" (0 = nenhum)
static OrigemCmd ligaOrigem   = ORIGEM_SERIAL;  // de onde veio o primeiro "l": o segundo tem de vir do mesmo lugar
#endif

Print &aviso() {
  if (logMs > 0) con().print("# ");
  return con();
}

char *marcaTempo(char *buf, size_t n, uint32_t ms) {
  snprintf(buf, n, "[t=%lu.%lu s] ", (unsigned long)(ms / 1000), (unsigned long)((ms % 1000) / 100));
  return buf;
}
Print &comTempo(Print &p, uint32_t ms) {
  char b[24];
  p.print(marcaTempo(b, sizeof(b), ms));
  return p;
}
Print &comTempo(Print &p) { return comTempo(p, millis()); }

// Aceita so numeros (ex.: "90", "45.5", "-3"). Evita que um texto errado
// vire 0 (alfa = 0 seria potencia maxima!). (Igual ao firmware dos SCRs.)
bool lerNumero(String arg, float &valor) {
  arg.trim();
  if (arg.length() == 0) return false;
  bool temDigito = false;
  for (unsigned int i = 0; i < arg.length(); i++) {
    char ch = arg.charAt(i);
    if (isdigit((unsigned char)ch)) temDigito = true;
    else if (!(ch == '.' || (i == 0 && (ch == '-' || ch == '+')))) return false;
  }
  if (!temDigito) return false;
  valor = arg.toFloat();
  return true;
}

// --------------------------------------------------------------------
void ajuda() {
  con().println("Comandos (115200 baud, \"Nova linha\"; aceita virgula ou ponto):");
  con().println("  modo boost | buck | nc | meia | ponte   troca o modo (desliga as saidas antes)");
#if PAINEL_MANUAL
  con().println("  l            (letra L) liga a saida. Com os pots: so com o pot do modo no inicio");
  con().println("  0            (zero) desliga TUDO na hora");
  con().println("  pot 1        controle pelos potenciometros (padrao) | pot 0  controle pela serial");
  con().println("  d 0.30       D do Boost/Buck (com pot 0 e saida ligada; aceita % : d 30)");
  con().println("  a 90         alfa em graus (com pot 0 e disparo ligado; 179 ou mais = sem disparo)");
#else
  con().println("  l            (letra L) liga a saida: mande l DUAS vezes (a segunda em ate 5 s). Parte de D = 0 / alfa = 180");
  con().println("  0            (zero) desliga TUDO na hora");
  con().println("  d 0.30       D do Boost/Buck (com a saida ligada; aceita % : d 30)");
  con().println("  a 90         alfa em graus (com o disparo ligado; 179 ou mais = sem disparo)");
#endif
  con().println("  tela auto    a tela OLED troca de pagina sozinha | tela 0, tela 1 ou tela 2 fixa uma pagina");
  con().println("  f 100000     frequencia do PWM (Boost 20000-150000, Buck 20000-100000); desliga a saida");
  con().println("  p 500        largura do pulso de gate dos SCRs (100 a 2000 us)");
  con().println("  rampa 1      rampa de alfa em graus por semiciclo (0 = sem rampa)");
  con().println("  r 47         resistencia da carga do retificador, em Ohm (Io = Vo/R)");
  con().println("  zero         zero do ACS712 (saida desligada e SEM corrente na carga)");
  con().println("  cal          mostra a calibracao | cal rsh1 0.1 | cal rsh2 0.1 | cal div 13.2 | cal acs 0.185 | cal padrao");
  con().println("  i2c          procura os dispositivos I2C de novo (com a saida desligada)");
  con().println("  lim          limites das protecoes | lim padrao  volta ao config.h");
  con().println("  lim io 0.2   limite de TESTE de Io (Buck) | lim iin 0.2  de Iin (Boost e Buck). So baixa, so na RAM");
  con().println("  log 1000     uma linha CSV a cada 1000 ms (log 0 desliga)");
  con().println("  wifi         rede, senha e endereco do painel web");
  con().println("  grava 1000   registro CSV na memoria (PSRAM), uma linha a cada 1000 ms | grava 0 para");
  con().println("  registro     estado do registro | registro apaga  apaga a memoria");
  con().println("  salva        copia o registro para a flash (so com a saida desligada)");
  con().println("  ?            estado e medidas | ajuda  esta lista");
}

// --------------------------------------------------------------------
static void secaoConversor(const Resultados &r) {
  Modo m = modoAtual();
  float d = pwmDAtual();
  con().printf("PWM: f = %lu Hz | %u bits | D atual = %.3f | D alvo = %.3f | DMAX = %.2f | ton = %.2f us\n",
                (unsigned long)pwmFrequencia(), (unsigned)pwmBits(), d, pwmDAlvo(), pwmDMax(),
                d * 1e6f / (float)pwmFrequencia());
  if (m == MODO_BUCK) con().printf("Driver IRS2104: %s (" TXT_SD_BUCK " = SD)\n", pwmLigado() ? "LIGADO" : "desligado");

  if (inaPresente(0)) {
    con().printf("INA226 #1 (0x40, entrada): Vin = %.3f V | Iin = %.4f A | Vshunt = %.3f mV\n",
                  r.vin, r.iin, r.vShunt1 * 1000.0f);
  } else {
    con().println("INA226 #1 (0x40, entrada): NAO responde");
  }
  if (inaPresente(1)) {
    con().printf("INA226 #2 (0x41, saida):   Vo  = %.3f V | Io  = %.4f A | Vshunt = %.3f mV\n",
                  r.vo, r.io, r.vShunt2 * 1000.0f);
  } else {
    con().println("INA226 #2 (0x41, saida):   NAO responde");
  }
  con().printf("Pin = %.3f W | Pout = %.3f W | eta = Pout/Pin = %.1f %%   (medias de 1 s)\n", r.pin, r.pout, r.eta);

  if (!isnan(r.vin)) {
    float vo, voAtraso, dq1;
    teoVoConversor(m == MODO_BOOST, r.vin, d, vo, voAtraso, dq1);
    if (m == MODO_BOOST) {
      if (!isnan(vo)) con().printf("Vo teorica (ideal) = Vin/(1-D) = %.2f V\n", vo);
    } else {
      con().printf("Vo teorica (ideal) = D x Vin = %.2f V | com o atraso tipico do IRS2104 (D no Q1 ~ %.3f): %.2f V\n",
                    vo, dq1, voAtraso);
    }
  }
}

void teoVoConversor(bool boost, float vin, float d, float &vo, float &voAtraso, float &dq1) {
  vo = voAtraso = dq1 = NAN;
  if (isnan(vin)) return;
  if (boost) {
    if (d < 0.99f) vo = vin / (1.0f - d);
    return;
  }
  dq1 = d - BUCK_ENCURTAMENTO_Q1_US * 1e-6f * (float)pwmFrequencia();
  if (dq1 < 0.0f) dq1 = 0.0f;
  vo = d * vin;
  voAtraso = dq1 * vin;
}

static void secaoRetificador(const Resultados &r) {
  Modo m = modoAtual();
  if (modoScr(m)) {
    scrMostrarEstado();
    // RMS teorico (carga R, ideal): a conta esta no disparo_scr.cpp (a pagina usa a mesma)
    ScrResumo s = scrResumo();
    float a = s.armado ? s.alfaAtual : 180.0f;
    float vrms = scrVoRmsTeorica(s.modo, a);
    con().printf("Vo RMS teorica (carga R, ideal) com alfa = %.1f graus: %.2f V\n", a, vrms);
  } else {
    ScrResumo s = scrResumo();
    if (!s.zcPresente) con().println("Rede: SEM SINAL DE ZC no " TXT_ZC " (a media fica simples, sem referencia de fase)");
    else con().printf("Rede: %s | f = %.2f Hz\n", s.sincronizado ? "SINCRONIZADO" : "NAO sincronizado", s.freqRede);
    con().printf("Teoricos (ideal, Vm = %.2f V): meia onda Vo_med = Vm/pi = %.2f V, Vo_rms = Vm/2 = %.2f V | "
                  "ponte Vo_med = 2Vm/pi = %.2f V, Vo_rms = Vm/raiz(2) = %.2f V\n",
                  VM_PICO, VM_PICO / (float)PI, VM_PICO / 2.0f, 2.0f * VM_PICO / (float)PI, VM_PICO / sqrtf(2.0f));
  }

  if (!adsPresente()) {
    con().println("ADS1115 (0x48): AUSENTE -> sem medidas do retificador (depois de ligar, use \"i2c\").");
    return;
  }
  con().printf("ADS1115: A0 = %.4f V (maior amostra %.3f V) | A1 = %.4f V | %u + %u amostras em 1 s\n",
                r.vA0, r.vA0max, r.vA1, (unsigned)r.nA0, (unsigned)r.nA1);
  if (r.porFase) {
    con().printf("  metodo: tempo equivalente (amostras ordenadas pela fase da rede; maior lacuna = %.1f graus)\n",
                  r.lacunaGraus);
  } else {
    con().println("  metodo: media simples (sem sincronismo com a rede: menos preciso)");
  }
  if (r.vA0max > ADS_LIMITE_A0_V) {
    con().println("ATENCAO: o A0 chegou perto de 3,3 V: os picos podem estar sendo cortados (veja o divisor no LEIA-ME).");
  }
  con().printf("Vo: media = %.3f V | RMS = %.3f V\n", r.voMed, r.voRms);
  con().printf("Io (ACS712): media = %.4f A | RMS = %.4f A\n", r.ioMed, r.ioRms);
  con().printf("Io pela carga (Vo/R, R = %.2f Ohm): media = %.4f A | RMS = %.4f A\n", cal.rCarga, r.ioRMed, r.ioRRms);
  con().printf("Pdc = Vo_med x Io_med = %.3f W | Pac = Vo_rms x Io_rms = %.3f W | eta = Pdc/Pac = %.1f %%\n",
                r.pdc, r.pac, r.etaRet);
  if (medicaoZeroEmAndamento()) con().println("(zero do ACS712 em andamento)");
}

static const char *textoControle() {
#if PAINEL_MANUAL
  return controlePeloPot() ? "pelos POTENCIOMETROS" : "pela SERIAL";
#else
  return webNoControle() ? "pela PAGINA WEB (um aparelho no controle) e pela serial"
                         : "pela serial (nenhum aparelho no controle da pagina web)";
#endif
}

void mostrarEstado() {
  const Resultados &r = medicaoResultados();
  Modo m = modoAtual();
  con().println("------------------------------------------------------------");
  comTempo(con()).println("Firmware bancada v" FW_VERSAO " (" FW_DATA ")");
  con().printf("Modo %s | saida %s | controle %s\n", nomeModo(m), saidaLigada() ? "LIGADA" : "DESLIGADA",
                textoControle());
  controleAvisoLimiteTeste(con(), false);   // "ATENCAO: limite de teste ativo: ..." (so se houver)
#if PAINEL_MANUAL
  con().printf("Pots: P1 = %.0f %% (pede D = %.3f) | P2 = %.0f %% (pede alfa = %.0f graus) | P3 = %.0f %% | tela: pagina %u\n",
                100.0f * potPosicao(POT_D), potD(modoConversor(m) ? pwmDMax() : BOOST_DMAX),
                100.0f * potPosicao(POT_ALFA), potAlfa(), 100.0f * potPosicao(POT_3), (unsigned)telaPagina());
  if (controlePotForaDoZero()) con().println("  Para ligar: volte o potenciometro do modo para o inicio do curso.");
#else
  con().printf("Tela OLED: pagina %u (%s) | bancada sem potenciometros e sem botoes (PAINEL_MANUAL 0)\n",
                (unsigned)telaPagina(), telaEhAuto() ? "troca sozinha" : "fixa");
#endif

  if (modoConversor(m)) secaoConversor(r);
  else secaoRetificador(r);

  calMostrar();
  con().printf("I2C: INA226 #1 %s | INA226 #2 %s | ADS1115 %s | OLED %s\n",
                inaPresente(0) ? "ok" : "AUSENTE", inaPresente(1) ? "ok" : "AUSENTE",
                adsPresente() ? "ok" : "AUSENTE", telaPresente() ? "ok" : "AUSENTE");
  const char *prot = controleUltimaProtecao();
  char mt[24];   // com a mesma marca da linha "PROTECAO: ...", para achar na serial
  if (prot[0]) con().printf("Ultimo desligamento automatico: %s%s (ha %lu s)\n",   // "ha" = ha quanto tempo
                            marcaTempo(mt, sizeof(mt), controleUltimaProtecaoMs()), prot,
                            (unsigned long)((millis() - controleUltimaProtecaoMs()) / 1000));
  else con().println("Ultimo desligamento automatico: nenhum");
  if (logMs > 0) con().printf("Log CSV ligado: uma linha a cada %lu ms\n", (unsigned long)logMs);
}

// --------------------------------------------------------------------
//                               Log CSV
// --------------------------------------------------------------------
static void campo(String &s, float v, unsigned int dec) {
  s += ';';
  if (!isnan(v) && !isinf(v)) s += String(v, dec);
}

const char *csvCabecalho() {
  return "tempo_ms;modo;D;alfa;Vin;Iin;Vo;Io;Pin;Pout;eta;Vo_med;Vo_rms;Io_med;Io_rms;f_rede";
}

static void cabecalhoLog() {
  con().println(csvCabecalho());
}

String csvLinha() {
  const Resultados &r = medicaoResultados();
  Modo m = modoAtual();
  ScrResumo sr = scrResumo();
  String s;
  s.reserve(200);
  s += String((unsigned long)millis());
  s += ';';
  s += nomeModo(m);
  campo(s, modoConversor(m) ? pwmDAtual() : NAN, 3);
  float alfa = NAN;
  if (modoScr(m)) alfa = sr.armado ? sr.alfaAtual : 180.0f;  // desligado = sem conducao = 180
  campo(s, alfa, 1);
  // Colunas que nao se aplicam ao modo ficam vazias: INA226 so nos
  // conversores, ADS1115 so nos retificadores.
  bool conv = modoConversor(m);
  campo(s, conv ? r.vin : NAN, 3);
  campo(s, conv ? r.iin : NAN, 4);
  campo(s, conv ? r.vo : NAN, 3);
  campo(s, conv ? r.io : NAN, 4);
  campo(s, conv ? r.pin : NAN, 3);
  campo(s, conv ? r.pout : NAN, 3);
  campo(s, conv ? r.eta : r.etaRet, 2);
  campo(s, conv ? NAN : r.voMed, 3);
  campo(s, conv ? NAN : r.voRms, 3);
  campo(s, conv ? NAN : r.ioMed, 4);
  campo(s, conv ? NAN : r.ioRms, 4);
  campo(s, sr.sincronizado ? sr.freqRede : NAN, 3);
  return s;
}

static void linhaLog() {
  con().println(csvLinha());
}

// --------------------------------------------------------------------
//                          Tratamento dos comandos
// --------------------------------------------------------------------
static const char *nomeDispositivo(uint8_t e) {
  switch (e) {
    case END_OLED:        return "OLED SSD1306";
    case 0x3D:            return "OLED (endereco alternativo 0x3D: mude END_OLED no config.h)";
    case END_INA_ENTRADA: return "INA226 #1 (entrada do conversor)";
    case END_INA_SAIDA:   return "INA226 #2 (saida do conversor)";
    case 0x44:
    case 0x45:            return "INA226? (confira os jumpers A0/A1: o #2 deve ficar em 0x41)";
    case END_ADS:         return "ADS1115";
    case 0x49:
    case 0x4A:
    case 0x4B:            return "ADS1115? (pino ADDR fora do GND: deve ficar em 0x48)";
    default:              return "desconhecido";
  }
}

static void varrerI2c() {
  if (saidaLigada()) {
    con().println("Desligue a saida (0) antes do i2c.");
    return;
  }
  comTempo(con()).printf("Procurando dispositivos I2C (SDA = GPIO%d, SCL = GPIO%d)...\n", PINO_SDA, PINO_SCL);
  int n = 0;
  for (uint8_t e = 0x08; e <= 0x77; e++) {
    if (i2cResponde(e)) {
      n++;
      con().printf("  0x%02X  %s\n", e, nomeDispositivo(e));
    }
  }
  if (n == 0) con().println("  Nenhum respondeu: confira 3,3 V, GND, SDA, SCL e os resistores de pull-up dos modulos.");
  medicaoDetectar(true);
  telaDetectar(true);
}

static void comandoModo(const String &arg) {
  Modo m;
  if (arg == "boost") m = MODO_BOOST;
  else if (arg == "buck") m = MODO_BUCK;
  else if (arg == "nc" || arg == "ret_nc") m = MODO_RET_NC;
  else if (arg == "meia" || arg == "ret_meia") m = MODO_RET_MEIA;
  else if (arg == "ponte" || arg == "ret_ponte") m = MODO_RET_PONTE;
  else {
    con().printf("Modo atual: %s. Use: modo boost | buck | nc | meia | ponte\n", nomeModo(modoAtual()));
    return;
  }
  controleMudarModo(m);
}

// Leituras cruas de todos os sensores (medias de 1 s), em qualquer modo:
// e o que se usa para calcular as calibracoes (ver LEIA-ME, secao Calibracao).
static void leiturasCruas() {
  const Resultados &r = medicaoResultados();
  con().printf("Agora (medias de 1 s): INA226 #1: Vbus = %.3f V, Vshunt = %.4f mV -> I = %.4f A\n",
                r.vin, r.vShunt1 * 1000.0f, r.iin);
  con().printf("                       INA226 #2: Vbus = %.3f V, Vshunt = %.4f mV -> I = %.4f A\n",
                r.vo, r.vShunt2 * 1000.0f, r.io);
  con().printf("                       ADS1115:   A0 = %.4f V -> Vo_med = %.3f V | A1 = %.4f V -> Io_med = %.4f A\n",
                r.vA0, r.voMed, r.vA1, r.ioMed);
}

static void comandoCal(const String &arg) {
  int esp = arg.indexOf(' ');
  String sub = (esp < 0) ? arg : arg.substring(0, esp);
  String val = (esp < 0) ? String("") : arg.substring(esp + 1);
  float v = 0.0f;
  if (sub.length() == 0) {
    comTempo(con());   // a marca vai no inicio da primeira linha ("Calibracao: ...")
    calMostrar();
    leiturasCruas();
    return;
  }
  if (sub == "padrao") {
    calPadrao();
    comTempo(con()).println("Calibracao voltou aos valores padrao do config.h.");
    calMostrar();
    return;
  }
  ItemCal item;
  const char *nome;
  const char *faixa;
  if (sub == "rsh1") { item = CAL_RSH1; nome = "Rsh1 (shunt do INA226 #1)"; faixa = "0.001 a 10 Ohm"; }
  else if (sub == "rsh2") { item = CAL_RSH2; nome = "Rsh2 (shunt do INA226 #2)"; faixa = "0.001 a 10 Ohm"; }
  else if (sub == "div") { item = CAL_DIV; nome = "Razao do divisor do A0"; faixa = "1 a 100"; }
  else if (sub == "acs") { item = CAL_ACS; nome = "Sensibilidade do ACS712 (V/A)"; faixa = "0.01 a 2 V/A (negativo inverte o sentido)"; }
  else {
    con().println("Use: cal | cal rsh1 <Ohm> | cal rsh2 <Ohm> | cal div <razao> | cal acs <V/A> | cal padrao");
    return;
  }
  if (!lerNumero(val, v)) {
    con().printf("Falta o valor. Ex.: cal %s 0.1\n", sub.c_str());
    return;
  }
  if (!calDefinir(item, v)) {
    con().printf("Valor fora da faixa aceita (%s).\n", faixa);
    return;
  }
  comTempo(con()).printf("%s = %.5f (vale ja; grava na memoria quando a saida estiver desligada)\n", nome, v);
  if (item == CAL_RSH1 || item == CAL_RSH2) {   // o fundo de escala e os limites efetivos seguem o Rsh
    calMostrarFundoEscala();
    con().println("Os limites de corrente seguem o Rsh novo: veja \"lim\".");
  }
}

// "lim" | "lim io <A>" | "lim iin <A>" | "lim padrao". Vale pela serial e pela caixa
// de comandos da pagina (os botoes da pagina nao mandam lim). A logica fica no controle.cpp.
static void comandoLim(const String &arg) {
  int esp = arg.indexOf(' ');
  String sub = (esp < 0) ? arg : arg.substring(0, esp);
  String val = (esp < 0) ? String("") : arg.substring(esp + 1);
  float v = 0.0f;
  if (sub.length() == 0) { controleMostrarLimites(); return; }
  if (sub == "padrao") { controleLimitesPadrao(); return; }
  LimiteCorrente qual;
  if (sub == "io") qual = LIM_IO;
  else if (sub == "iin") qual = LIM_IIN;
  else {
    con().println("Use: lim | lim io <A> | lim iin <A> | lim padrao");
    return;
  }
  if (!lerNumero(val, v)) {
    con().printf("Falta o valor em A. Ex.: lim %s 0.20\n", sub.c_str());
    return;
  }
  controleDefinirLimite(qual, v);
}

static void comandoLog(const String &arg) {
  float v = 0.0f;
  if (!lerNumero(arg, v)) {
    if (logMs > 0) con().printf("Log ligado: uma linha a cada %lu ms. \"log 0\" desliga.\n", (unsigned long)logMs);
    else con().println("Use: log <ms> (ex.: log 1000) | log 0 desliga");
    return;
  }
  if (v <= 0.0f) {
    if (logMs > 0) con().println("# Log desligado.");
    logMs = 0;
    return;
  }
  if (v > (float)LOG_MAX_MS) v = (float)LOG_MAX_MS;
  uint32_t ms = (uint32_t)lroundf(v);
  if (ms < LOG_MIN_MS) ms = LOG_MIN_MS;
  logMs = ms;
  ultimoLogMs = millis();
  con().printf("# Log CSV a cada %lu ms (valores = medias de 1 s). \"log 0\" para parar.\n", (unsigned long)ms);
  cabecalhoLog();
}

// "tela auto" | "tela 0" | "tela 1" | "tela 2"
static void comandoTela(const String &arg) {
  float v = 0.0f;
  if (arg == "auto") {
    telaAuto();
    con().printf("Tela OLED: troca de pagina sozinha a cada %lu s.\n", (unsigned long)(TELA_TROCA_MS / 1000));
    return;
  }
  if (lerNumero(arg, v) && v >= 0.0f && v < (float)TELA_NUM_PAGINAS && v == floorf(v)) {
    telaDefinirPagina((uint8_t)v);
    con().printf("Tela OLED: pagina %u fixa (\"tela auto\" volta a trocar sozinha).\n", (unsigned)v);
    return;
  }
  con().printf("Tela OLED: pagina %u, %s. Use: tela 0 | tela 1 | tela 2 (fixa) ou tela auto (troca a cada %lu s)\n",
                (unsigned)telaPagina(), telaEhAuto() ? "trocando sozinha" : "fixa",
                (unsigned long)(TELA_TROCA_MS / 1000));
}

// "l": com os pots, liga direto (a trava e o pot no inicio). Sem os pots
// (PAINEL_MANUAL 0), a serial e a caixa de comandos da pagina pedem confirmacao:
// o primeiro "l" so avisa; o segundo, em ate LIGA_CONFIRMA_MS e do MESMO lugar,
// liga. O botao "Ligar a saida" da pagina ja confirmou na janela dele, entao o
// "l" que vem dele liga direto. Sempre parte de D = 0 / alfa = 180 graus (controle.cpp).
static void comandoLiga(OrigemCmd origem) {
#if PAINEL_MANUAL
  (void)origem;
  controleLigar();
#else
  uint32_t agora = millis();
  Modo m = modoAtual();
  bool confirmado = ligaPedidoMs != 0 && ligaOrigem == origem && agora - ligaPedidoMs <= LIGA_CONFIRMA_MS;
  ligaPedidoMs = 0;
  if (origem == ORIGEM_WEB_BOTAO || confirmado || saidaLigada() || !(modoConversor(m) || modoScr(m))) {
    controleLigar();   // ja ligada ou RET_NC: so responde; senao, liga
    return;
  }
  ligaPedidoMs = agora | 1u;   // nunca 0
  ligaOrigem = origem;
  con().printf("Ligar a saida no modo %s? Ela comeca em %s e so sobe quando voce mandar %s. "
                "Para confirmar, mande l de novo em ate %lu s (outro comando cancela).\n",
                nomeModo(m), modoConversor(m) ? "D = 0" : "alfa = 180 graus (sem disparo)",
                modoConversor(m) ? "d" : "a", (unsigned long)(LIGA_CONFIRMA_MS / 1000));
  char prazo[20];
  snprintf(prazo, sizeof(prazo), "em ate %lu s", (unsigned long)(LIGA_CONFIRMA_MS / 1000));
  telaMensagem("Ligar a saida?", "mande l de novo", prazo, LIGA_CONFIRMA_MS);
#endif
}

static void tratarComando(String s, OrigemCmd origem) {
  s.trim();
  s.replace(',', '.');
  s.toLowerCase();
  if (s.length() == 0) return;

  if (s == "0") {
#if !PAINEL_MANUAL
    ligaPedidoMs = 0;
#endif
    controleDesligar(nullptr);
    return;
  }

  int esp = s.indexOf(' ');
  String cmd = (esp < 0) ? s : s.substring(0, esp);
  String arg = (esp < 0) ? String("") : s.substring(esp + 1);
  arg.trim();
  float v = 0.0f;

  if (cmd == "l" || cmd == "liga") { comandoLiga(origem); return; }
#if !PAINEL_MANUAL
  ligaPedidoMs = 0;   // qualquer outro comando cancela um "l" esperando confirmacao
#endif
  if (cmd == "?") { mostrarEstado(); return; }
  if (cmd == "ajuda" || cmd == "help") { ajuda(); return; }
  if (cmd == "modo") { comandoModo(arg); return; }
  if (cmd == "tela") { comandoTela(arg); return; }
  if (cmd == "pot") {
#if PAINEL_MANUAL
    if (arg == "1") controleUsarPot(true);
    else if (arg == "0") controleUsarPot(false);
    else con().println("Use: pot 1 (potenciometros) ou pot 0 (serial)");
#else
    controleUsarPot(arg == "1");   // sem pots: "pot 1" recusado com aviso
#endif
    return;
  }
  if (cmd == "zero") { controleZero(); return; }
  if (cmd == "cal") { comandoCal(arg); return; }
  if (cmd == "i2c") { varrerI2c(); return; }
  if (cmd == "lim") { comandoLim(arg); return; }
  if (cmd == "log") { comandoLog(arg); return; }
  if (cmd == "wifi") { webMostrar(); return; }
  if (cmd == "grava") {
    if (!lerNumero(arg, v) || v < 0) { con().println("Use: grava <ms> (ex.: grava 1000) | grava 0 para"); return; }
    if (v == 0) { registroPedirParada(); aviso().println("Registro: parada pedida."); }
    else { registroPedirInicio((uint32_t)v); aviso().printf("Registro: gravando uma linha a cada %lu ms (limites %lu a %lu ms).\n", (unsigned long)v, (unsigned long)LOG_MIN_MS, (unsigned long)LOG_MAX_MS); }
    return;
  }
  if (cmd == "registro") {
    if (arg == "apaga") { registroPedirApagar(); aviso().println("Registro: apagado."); return; }
    aviso().printf("Registro: %s, %lu linhas, %.1f de %u kB%s. Painel: baixe em http://192.168.4.1/registro.csv\n",
                   registroGravando() ? "GRAVANDO" : "parado", (unsigned long)registroLinhas(),
                   registroBytes() / 1024.0, (unsigned)(registroCapacidade() / 1024), registroCheio() ? " (CHEIO)" : "");
    if (registroTemSalvo()) aviso().printf("Copia na flash: %u bytes\n", (unsigned)registroTamanhoSalvo());
    if (registroMensagem()[0]) aviso().printf("Ultima mensagem: %s\n", registroMensagem());
    return;
  }
  if (cmd == "salva") {
    if (registroPedirSalvar()) aviso().println("Registro: salvando na flash (aguarde alguns segundos e use \"registro\").");
    else aviso().println("Desligue a saida antes de salvar na flash.");
    return;
  }
  if (cmd == "rampa") {
    if (!lerNumero(arg, v)) {
      con().printf("Rampa de alfa = %.2f grau/semiciclo. Use: rampa <graus>  (rampa 0 = sem rampa)\n", scrRampa());
      return;
    }
    scrDefinirRampa(v);
    if (scrRampa() <= 0.0f) con().println("Rampa desligada: alfa muda direto no proximo semiciclo.");
    else con().printf("Rampa = %.2f grau por semiciclo (%.0f graus por segundo em 60 Hz)\n", scrRampa(), scrRampa() * 120.0f);
    return;
  }

  // Comandos de uma letra seguida de numero: aceita "d 0.3" e "d0.3".
  char c = s.charAt(0);
  if (esp < 0) {
    arg = s.substring(1);
    arg.trim();
  }
  switch (c) {
    case 'd':
      if (!lerNumero(arg, v)) con().println("Use: d <D>   ex.: d 0.3   ou   d 30 (em %)");
      else controleDefinirD(v);
      return;
    case 'a':
      if (!lerNumero(arg, v)) con().println("Use: a <graus>   ex.: a 90   ou   a 45,5");
      else controleDefinirAlfa(v);
      return;
    case 'f':
      if (!lerNumero(arg, v) || v <= 0.0f) con().println("Use: f <Hz>   ex.: f 100000");
      else controleDefinirFrequencia((uint32_t)lroundf(fminf(v, 1e7f)));  // a faixa e limitada depois
      return;
    case 'p':
      if (!lerNumero(arg, v)) {
        con().println("Use: p <us>   ex.: p 500");
      } else {
        v = fminf(fmaxf(v, 0.0f), 1e6f);  // depois fica entre 100 e 2000 us
        scrDefinirPulso((uint32_t)lroundf(v));
        con().printf("Largura do pulso de gate = %lu us (vale a partir do proximo semiciclo)\n",
                      (unsigned long)scrResumo().pulso_us);
      }
      return;
    case 'r':
      if (!lerNumero(arg, v)) {
        con().printf("R da carga = %.2f Ohm. Use: r <Ohm>   ex.: r 47\n", cal.rCarga);
      } else if (!calDefinir(CAL_RCARGA, v)) {
        con().println("Valor fora da faixa (0.1 a 100000 Ohm).");
      } else {
        comTempo(con()).printf("R da carga = %.2f Ohm (Io = Vo/R; grava na memoria com a saida desligada)\n", cal.rCarga);
      }
      return;
    default:
      break;
  }
  con().println("Comando nao reconhecido. Digite ajuda.");
}

// Le a serial SEM travar o loop (junta os caracteres ate chegar o Enter).
static void lerSerial() {
  static String linha;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (linha.length() > 0) {
        // So no espelho (a serial nao muda): a pagina mostra o comando antes da resposta.
        String ecoSerial = String(logMs > 0 ? "# " : "") + "[serial] " + linha + "\r\n";
        espelhoGuardar((const uint8_t *)ecoSerial.c_str(), ecoSerial.length());
        tratarComando(linha, ORIGEM_SERIAL);
        linha = "";
      }
    } else if (linha.length() < LINHA_MAX) {
      linha += ch;
    }
  }
}

// Comando vindo do painel web (chamado no loop, nucleo 1, pelo web.cpp). Na serial
// aparece "[painel web] <comando>" e depois a resposta, como se fosse digitado la.
String comandoWeb(const char *txt, bool caixa) {
  if (logMs > 0) tee.print("# ");   // com o log ligado, nao mistura com as linhas do CSV
  tee.printf("[painel web] %s\n", txt);
  eco.n = 0;
  eco.cortado = false;
  eco.capturando = true;
  tratarComando(String(txt), caixa ? ORIGEM_WEB_CAIXA : ORIGEM_WEB_BOTAO);
  eco.capturando = false;
  if (eco.cortado) {   // guardar() deixou lugar para o aviso
    memcpy(eco.buf + eco.n, ECO_CORTE, sizeof(ECO_CORTE) - 1);
    eco.n += sizeof(ECO_CORTE) - 1;
  }
  eco.buf[eco.n] = 0;
  return String(eco.buf);
}

// --------------------------------------------------------------------
void comandosCancelarLiga() {
#if !PAINEL_MANUAL
  ligaPedidoMs = 0;
#endif
}

void comandosIniciar() {
  logMs = 0;
  comandosCancelarLiga();
}

void comandosAtualizar() {
  lerSerial();
  if (logMs > 0) {
    uint32_t agora = millis();
    if (agora - ultimoLogMs >= logMs) {
      ultimoLogMs += logMs;
      if (agora - ultimoLogMs >= logMs) ultimoLogMs = agora;  // atrasou muito: nao tenta "alcancar"
      linhaLog();
    }
  }
}
