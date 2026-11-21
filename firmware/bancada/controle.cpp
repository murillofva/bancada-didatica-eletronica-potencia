/*
  controle.cpp - Modos, liga/desliga com trava, potenciometros x serial,
  protecoes dos conversores e LEDs.

  TRAVA DE SEGURANCA (controle pelos potenciometros, com PAINEL_MANUAL 1):
    LIGA so funciona com o potenciometro do modo no INICIO do curso
    (Pot 1 -> D = 0 nos conversores; Pot 2 -> alfa = 180 graus nos SCRs).
    Assim a saida sempre parte do zero e sobe quando o aluno gira o pot.
  Controle pela serial ("pot 0"): o LIGA (botao ou "l") sempre parte do
  valor seguro (D = 0 / alfa = 180); depois o valor e pedido com "d" ou "a".

  SEM POTENCIOMETROS E SEM BOTOES (PAINEL_MANUAL 0, o padrao):
    o controle e sempre "pela serial" (serial e pagina web). Ligar SEMPRE parte
    de D = 0 / alfa = 180 graus; a pagina pede confirmacao e a serial pede o
    "l" duas vezes (comandos.cpp). "pot 1" e recusado com aviso.
  Desligar (comando, troca de modo, protecao, homem-morto da pagina) volta os
  alvos para D = 0 e alfa = 180: nada religa sozinho.

  LIMITES DE TESTE (comando "lim"): baixam so na RAM os limites
  de corrente das protecoes, para testa-las com corrente baixa. Ver config.h.

  FUNDO DE ESCALA DOS INA226: o registrador do shunt satura em
  81,92 mV, e a corrente lida trava em 81,92 mV / Rsh. Por isso:
    - Vshunt no fundo de escala (#1 ou #2), com a saida ligada, desliga a saida;
    - cada limite de corrente fica no menor entre o config.h e 95 % do fundo de
      escala calibrado (o "limite efetivo"), e o "lim" so baixa a partir dele.
*/
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "controle.h"
#include "pwm_conversores.h"
#include "disparo_scr.h"
#include "medicao.h"
#include "painel.h"
#include "tela.h"
#include "comandos.h"

static Modo     modo           = MODO_RET_NC;
#if PAINEL_MANUAL
static bool     peloPot        = true;   // padrao: pelos potenciometros
#else
static const bool peloPot      = false;  // sem pots: sempre pela serial e pela pagina (= "pot 0")
#endif
static char     ultimaProtecao[48] = "";
static uint32_t ultimaProtecaoMs    = 0;   // millis() do ultimo desligamento automatico
// O aviso da pagina e da tela OLED vale so ate religar a saida ou pegar o controle
// (senao um desligamento antigo pareceria atual).
static bool     protecaoAtual  = false;
static uint32_t ultimoLedMs    = 0;

// Limites de teste do comando "lim" (so na RAM). 0 = sem teste: vale o limite efetivo.
static float    limIinTeste    = 0.0f;   // "lim iin": Iin do Boost e do Buck
static float    limIoTeste     = 0.0f;   // "lim io":  Io do Buck
static const float LIM_FOLGA   = 0.0005f; // comparacao de floats (os valores andam de 0,01 em 0,01 A)
static const char  TXT_LIM_TESTE[] = "ATENCAO: limite de teste ativo: %s. \"lim padrao\" ou reset volta ao config.h.\n";
static const char  TXT_LIM_LIGADA[] = "NAO mudei: desligue a saida (0) antes de mudar os limites.";

// Teto que o INA226 #k permite: LIM_FRACAO_FUNDO do fundo de escala calibrado
// (81,92 mV / Rsh), arredondado PARA BAIXO em 0,01 A (k = 0: #1, Iin; k = 1: #2, Io).
static float limSensor(uint8_t k) {
  return floorf(LIM_FRACAO_FUNDO * medicaoFundoEscala(k) * 100.0f + 1e-3f) / 100.0f;
}
// Limite efetivo: o menor entre o config.h e o teto do INA226 que mede a corrente.
static float efBoostIin() { return fminf(BOOST_IIN_MAX, limSensor(0)); }
static float efBuckIin()  { return fminf(BUCK_IIN_MAX,  limSensor(0)); }
static float efBuckIo()   { return fminf(BUCK_IO_MAX,   limSensor(1)); }
// Limite ativo: o efetivo ou, se for menor, o de teste do "lim".
static float limBoostIin() { return limIinTeste > 0.0f ? fminf(limIinTeste, efBoostIin()) : efBoostIin(); }
static float limBuckIin()  { return limIinTeste > 0.0f ? fminf(limIinTeste, efBuckIin())  : efBuckIin(); }
static float limBuckIo()   { return limIoTeste  > 0.0f ? fminf(limIoTeste,  efBuckIo())   : efBuckIo(); }
static bool  abaixo(float a, float b) { return a < b - LIM_FOLGA; }

static const char *NOMES[NUM_MODOS]      = { "BOOST", "BUCK", "RET_NC", "RET_MEIA", "RET_PONTE" };
static const char *NOMES_TELA[NUM_MODOS] = { "BOOST", "BUCK", "RET NC", "RET MEIA", "RET PONTE" };
static const char *DESCRICAO[NUM_MODOS]  = {
  "conversor Boost (PWM no " TXT_PWM_BOOST " -> TC4427)",
  "conversor Buck sincrono (" TXT_PWM_BUCK " -> IN e " TXT_SD_BUCK " -> SD do IRS2104)",
  "retificador NAO controlado (diodos): so medicao",
  "meia onda controlada (1 SCR, so GATE_A)",
  "ponte semicontrolada (2 SCRs + 2 diodos, GATE_A e GATE_B)",
};

// --------------------------------------------------------------------
Modo modoAtual() { return modo; }
const char *nomeModo(Modo m) { return (m < NUM_MODOS) ? NOMES[m] : "?"; }
const char *nomeModoTela(Modo m) { return (m < NUM_MODOS) ? NOMES_TELA[m] : "?"; }
bool modoConversor(Modo m) { return m == MODO_BOOST || m == MODO_BUCK; }
bool modoScr(Modo m) { return m == MODO_RET_MEIA || m == MODO_RET_PONTE; }
bool controlePeloPot() { return peloPot; }
const char *controleUltimaProtecao() { return ultimaProtecao; }
uint32_t controleUltimaProtecaoMs() { return ultimaProtecaoMs; }
const char *controleProtecaoAtual() { return protecaoAtual ? ultimaProtecao : ""; }
void controleApagarProtecaoAtual() { protecaoAtual = false; }

bool saidaLigada() {
  if (modoConversor(modo)) return pwmLigado();
  if (modoScr(modo)) return scrResumo().armado;
  return false;
}

float controleAlvo() {
  if (modoConversor(modo)) return pwmDAlvo();
  if (modoScr(modo)) return scrResumo().alfaAlvo;
  return NAN;
}

bool controlePotForaDoZero() {
#if PAINEL_MANUAL
  if (!peloPot || saidaLigada()) return false;
  if (modoConversor(modo)) return !potNoZero(POT_D);
  if (modoScr(modo)) return !potNoZero(POT_ALFA);
#endif
  return false;
}

// --------------------------------------------------------------------
//                               Modos
// --------------------------------------------------------------------
void controleMudarModo(Modo m) {
  if (m >= NUM_MODOS) return;
  bool estava = saidaLigada();
  // Primeiro TUDO desligado, depois a troca.
  pwmDesligar();
  scrDesligar();
  modo = m;
  if (m == MODO_BOOST) pwmSelecionar(CONV_BOOST);
  if (m == MODO_BUCK) pwmSelecionar(CONV_BUCK);
  if (m == MODO_RET_MEIA) scrDefinirModo(1);
  if (m == MODO_RET_PONTE) scrDefinirModo(2);
  scrDefinirAlfa(180.0f, false);  // alvo seguro ate o pot/serial mandar outro
  comTempo(aviso()).printf("Modo %s: %s.%s\n", NOMES[m], DESCRICAO[m],
                 estava ? " Saida DESLIGADA por seguranca." : "");
  telaMensagem("Modo:", NOMES_TELA[m], estava ? "saida desligada" : nullptr, 1500);
}

#if PAINEL_MANUAL
void controleProximoModo() {
  controleMudarModo((Modo)((modo + 1) % NUM_MODOS));
}
#endif

// --------------------------------------------------------------------
//                            Liga / desliga
// --------------------------------------------------------------------
static bool ligarConversor() {
  if (!inaPresente(0) || !inaPresente(1)) {
#if EXIGIR_INA226
    aviso().println("NAO liguei: os dois INA226 precisam responder (sem eles nao ha protecao). Confira e use \"i2c\".");
    telaMensagem("LIGA recusado:", "INA226 ausente", "(sem protecao)");
    return false;
#else
    aviso().printf("ATENCAO: ligando sem %s%s%s (EXIGIR_INA226 = 0 no config.h). Sem protecao de corrente/tensao "
                   "nesse lado: limite a corrente na fonte.\n",
                   inaPresente(0) ? "" : "o INA226 #1 (entrada)", (!inaPresente(0) && !inaPresente(1)) ? " e " : "",
                   inaPresente(1) ? "" : "o INA226 #2 (saida)");
    telaMensagem("ATENCAO:", "sem INA226", "(sem protecao)", 2500);
#endif
  }
#if PAINEL_MANUAL
  if (peloPot) {
    if (!potNoZero(POT_D)) {
      aviso().printf("NAO liguei: gire o POT 1 ate o inicio (D = 0) antes de ligar. Agora ele pede D = %.3f.\n",
                     potD(pwmDMax()));
      telaMensagem("LIGA recusado:", "gire o POT 1", "ate D = 0");
      return false;
    }
    pwmDefinirAlvo(potD(pwmDMax()));
  } else {
    pwmDefinirAlvo(0.0f);
  }
#else
  pwmDefinirAlvo(0.0f);   // sem pots: sempre parte de D = 0
#endif
  pwmLigar();
  const char *quem = peloPot ? "girando o POT 1" : "com o comando d (ex.: d 0.3)";
  if (modo == MODO_BOOST) {
    comTempo(aviso()).printf("Boost LIGADO: f = %lu Hz, D comeca em 0. Suba D %s (rampa de 0,5 por segundo, D max = %.2f).\n",
                   (unsigned long)pwmFrequencia(), quem, pwmDMax());
  } else {
    comTempo(aviso()).printf("Buck LIGADO: driver ligado com D = 0 (Q2 ligado, bootstrap carregando). Confira: corrente da fonte ~0. Suba D %s.\n",
                   quem);
  }
  bool limTeste = controleAvisoLimiteTeste(aviso(), true);   // so os limites de teste que valem neste modo
  telaMensagem("Saida LIGADA", modo == MODO_BOOST ? "Boost, D = 0" : "Buck, D = 0",
               limTeste ? "LIMITE DE TESTE" : nullptr, limTeste ? 3000 : 1500);
  return true;
}

static bool ligarScr() {
#if PAINEL_MANUAL
  if (peloPot) {
    if (!potNoZero(POT_ALFA)) {
      aviso().printf("NAO liguei: gire o POT 2 ate o inicio (alfa = 180 graus) antes de ligar. Agora ele pede alfa = %.0f graus.\n",
                     potAlfa());
      telaMensagem("LIGA recusado:", "gire o POT 2", "ate alfa = 180");
      return false;
    }
    scrDefinirAlfa(potAlfa(), false);
  } else {
    scrDefinirAlfa(180.0f, false);
  }
#else
  scrDefinirAlfa(180.0f, false);   // sem pots: sempre parte de alfa = 180 graus (sem conducao)
#endif
  ScrResumo s = scrResumo();
  if (!scrLigar()) {
    if (!s.sincronizado) telaMensagem("LIGA recusado:", "rede sem sinc.", "(confira o ZC)");
    else telaMensagem("LIGA recusado:", "sem sinal POL", "(meia onda)");
    return false;
  }
  if (peloPot) aviso().println("Gire o POT 2 para diminuir alfa (mais tensao na carga).");
  else aviso().println("Agora mande o alfa com o comando a (ex.: a 90).");
  telaMensagem("Disparo LIGADO", "alfa = 180", nullptr, 1500);
  return true;
}

bool controleLigar() {
  if (saidaLigada()) {
    aviso().println("A saida ja esta ligada.");
    return true;
  }
  bool ok;
  if (modoConversor(modo)) ok = ligarConversor();
  else if (modoScr(modo)) ok = ligarScr();
  else {
    aviso().println("RET_NC e so medicao (diodos): nao ha o que ligar.");
    telaMensagem("RET NC:", "so medicao,", "nada a ligar");
    return false;
  }
  if (ok) protecaoAtual = false;   // religou: o aviso sai da pagina e da tela (o "?" guarda o ultimo)
  return ok;
}

static void desligarTudo() {
  pwmDesligar();   // Buck: SD em LOW primeiro; os dois PWMs em 0
  scrDesligar();   // gates em LOW, nada agendado
  pwmDefinirAlvo(0.0f);          // alvos de volta ao valor seguro: nada religa sozinho
  scrDefinirAlfa(180.0f, false); // (com os pots, o loop volta a copiar o valor do pot)
}

// Desligamento automatico. tela2/tela3: linhas da tela OLED (ate 19 caracteres).
static void desligarPorProtecao(const char *motivo, const char *tela2, const char *tela3) {
  desligarTudo();
  ultimaProtecaoMs = millis();
  snprintf(ultimaProtecao, sizeof(ultimaProtecao), "%s", motivo);
  protecaoAtual = true;
  // A marca de tempo vai no inicio da linha, como nas outras mensagens.
  comTempo(aviso(), ultimaProtecaoMs).printf("PROTECAO: %s -> saida DESLIGADA.\n", motivo);
  telaMensagem("DESLIGOU:", tela2, tela3, 5000);
}

void controleDesligar(const char *motivo) {
  if (motivo != nullptr) {
    desligarPorProtecao(motivo, motivo, nullptr);
    return;
  }
  bool estava = saidaLigada();
  desligarTudo();
  if (!estava) {
    aviso().println("A saida ja estava desligada (PWM em 0, SD em LOW, gates em LOW).");
    return;
  }
  if (modo == MODO_BUCK) comTempo(aviso()).println("Saida DESLIGADA: driver desligado (Q1 e Q2 desligados) e D = 0.");
  else if (modo == MODO_BOOST) comTempo(aviso()).println("Saida DESLIGADA: PWM do Boost em 0 (MOSFET desligado).");
  else comTempo(aviso()).println("Disparo DESLIGADO (GATE_A e GATE_B em LOW).");
  telaMensagem("Saida", "DESLIGADA", nullptr, 1500);
}

#if PAINEL_MANUAL
void controleAlternar() {
  if (saidaLigada()) controleDesligar(nullptr);
  else controleLigar();
}
#endif

// --------------------------------------------------------------------
//                       Potenciometro x serial
// --------------------------------------------------------------------
bool controleUsarPot(bool sim) {
#if !PAINEL_MANUAL
  if (sim) {
    con().println("NAO: esta bancada esta montada SEM potenciometros (PAINEL_MANUAL 0 no config.h). "
                  "O controle e pela pagina web e pela serial: use d <D> e a <graus>.");
    return false;
  }
  con().println("O controle ja esta pela serial e pela pagina web (bancada sem potenciometros).");
  return true;
#else
  if (sim == peloPot) {
    con().printf("O controle ja esta pel%s.\n", sim ? "os POTENCIOMETROS" : "a SERIAL");
    return true;
  }
  if (sim && saidaLigada()) {
    uint8_t k = modoConversor(modo) ? POT_D : POT_ALFA;
    if (!potNoZero(k)) {
      con().printf("NAO voltei para o potenciometro: com a saida ligada, gire o POT %u ate o inicio antes "
                    "(ou desligue a saida com 0).\n", (unsigned)(k + 1));
      return false;
    }
  }
  peloPot = sim;
  if (sim) con().println("Controle pelos POTENCIOMETROS (Pot 1 = D, Pot 2 = alfa).");
  else con().println("Controle pela SERIAL: use d <D> e a <graus>. O valor atual foi mantido.");
  return true;
#endif
}

bool controleDefinirD(float d) {
  if (!modoConversor(modo)) {
    con().println("O comando d so vale nos modos BOOST e BUCK.");
    return false;
  }
  if (peloPot) {
    con().println("O D esta no POT 1. Para usar a serial, mande \"pot 0\" antes.");
    return false;
  }
  if (!pwmLigado()) {
    con().println("Saida desligada: ligue com l (comeca em D = 0) e depois mande d.");
    return false;
  }
  if (d > 1.0f) d = d / 100.0f;  // aceita em %
  if (d < 0.0f) d = 0.0f;
  if (d > pwmDMax()) {
    con().printf("D pedido acima do limite; usando D = %.2f\n", pwmDMax());
    d = pwmDMax();
  }
  pwmDefinirAlvo(d);
  con().printf("Indo para D = %.3f (rampa)\n", pwmDAlvo());
  return true;
}

bool controleDefinirAlfa(float graus) {
  if (!modoScr(modo)) {
    con().println("O comando a so vale nos modos RET_MEIA e RET_PONTE.");
    return false;
  }
  if (peloPot) {
    con().println("O alfa esta no POT 2. Para usar a serial, mande \"pot 0\" antes.");
    return false;
  }
  if (!scrResumo().armado) {
    con().println("Disparo desligado: ligue com l (comeca em alfa = 180 graus) e depois mande a.");
    return false;
  }
  if (graus < 0.0f) graus = 0.0f;
  if (graus > 180.0f) graus = 180.0f;
  scrDefinirAlfa(graus, true);
  con().printf("alfa alvo = %.2f graus%s -> muda em rampa (%.1f grau/semiciclo) a partir do proximo semiciclo.\n",
                graus, graus >= ALFA_SEM_DISPARO / 100.0f ? " (179 ou mais = sem disparo)" : "", scrRampa());
  return true;
}

bool controleDefinirFrequencia(uint32_t hz) {
  if (!modoConversor(modo)) {
    con().println("O comando f so vale nos modos BOOST e BUCK.");
    return false;
  }
  bool estava = pwmLigado();
  bool ok = pwmDefinirFrequencia(hz);
  comTempo(con()).printf("Frequencia = %lu Hz (%u bits)%s. D voltou para 0%s.%s\n",
                (unsigned long)pwmFrequencia(), (unsigned)pwmBits(),
                (hz != pwmFrequencia()) ? " (limitada a faixa permitida)" : "",
                estava ? " e a saida foi DESLIGADA (ligue de novo)" : "",
                ok ? "" : " ERRO ao configurar o PWM!");
  return ok;
}

void controleZero() {
  if (!adsPresente()) {
    aviso().println("ADS1115 ausente: nao da para zerar o ACS712.");
    telaMensagem("ZERO recusado:", "ADS1115 ausente");
    return;
  }
  if (saidaLigada()) {
    aviso().println("Desligue a saida antes de zerar o ACS712 (precisa de corrente zero).");
    telaMensagem("ZERO recusado:", "desligue a saida");
    return;
  }
  medicaoPedirZero();
  comTempo(aviso()).println("Zero do ACS712: medindo por ~2 s. A corrente na carga precisa ser ZERO "
                  "(no RET_NC: trafo desligado ou carga solta).");
  telaMensagem("Zero do ACS712", "medindo (2 s)", "sem corrente!", 2500);
}

// --------------------------------------------------------------------
//                       Protecoes dos conversores
// --------------------------------------------------------------------
static void verificarProtecoes() {
  if (!medicaoInaNova()) return;                     // so quando ha leitura nova (~50 ms)
  if (!modoConversor(modo) || !pwmLigado()) return;
  const LeituraIna &e = medicaoIna(0);
  const LeituraIna &s = medicaoIna(1);
#if EXIGIR_INA226
  if (!e.ok) { controleDesligar("INA226 #1 mudo"); return; }
  if (!s.ok) { controleDesligar("INA226 #2 mudo"); return; }
#endif
  // com EXIGIR_INA226 = 0, cada limite so e testado se o INA226 dele estiver respondendo.
  char motivo[48];

  // 1) Vshunt no fundo de escala (#1 ou #2, nos dois conversores): a leitura travou em
  //    81,92 mV / Rsh e a corrente real pode ser bem maior. Vem antes dos limites para
  //    que a mensagem diga o que de fato aconteceu.
  for (uint8_t k = 0; k < 2; k++) {
    const LeituraIna &l = (k == 0) ? e : s;
    if (!l.ok || !l.saturado) continue;
    float fe = medicaoFundoEscala(k);
    char t2[20], t3[20];
    snprintf(motivo, sizeof(motivo), "INA226 #%u no fundo de escala (I >= %.2fA)", (unsigned)(k + 1), fe);
    snprintf(t2, sizeof(t2), "INA226 #%u saturou", (unsigned)(k + 1));
    snprintf(t3, sizeof(t3), "I >= %.2f A", fe);
    desligarPorProtecao(motivo, t2, t3);
    aviso().printf("Vshunt no fundo de escala (81.92 mV): a leitura trava em %.3f A e a corrente real pode ser maior. "
                   "Confira a carga e o D antes de ligar de novo.\n", fe);
    return;
  }

  // 2) Limites. Os de corrente sao os ativos: o efetivo ou o de teste do "lim".
  float limite = NAN, efetivo = NAN, doConfig = NAN;   // limite de corrente que desligou (para o aviso de teste)
  if (modo == MODO_BOOST) {
    if (s.ok && s.v > BOOST_VO_MAX) {
      snprintf(motivo, sizeof(motivo), "Vo %.1fV > %.0fV", s.v, BOOST_VO_MAX);
      controleDesligar(motivo);
    } else if (e.ok && fabsf(e.i) > limBoostIin()) {
      limite = limBoostIin();
      efetivo = efBoostIin();
      doConfig = BOOST_IIN_MAX;
      snprintf(motivo, sizeof(motivo), "Iin %.3fA > %.2fA", fabsf(e.i), limite);
      controleDesligar(motivo);
    }
  } else {
    if (e.ok && fabsf(e.i) > limBuckIin()) {
      limite = limBuckIin();
      efetivo = efBuckIin();
      doConfig = BUCK_IIN_MAX;
      snprintf(motivo, sizeof(motivo), "Iin %.3fA > %.2fA", fabsf(e.i), limite);
      controleDesligar(motivo);
    } else if (s.ok && fabsf(s.i) > limBuckIo()) {
      limite = limBuckIo();
      efetivo = efBuckIo();
      doConfig = BUCK_IO_MAX;
      snprintf(motivo, sizeof(motivo), "Io %.3fA > %.2fA", fabsf(s.i), limite);
      controleDesligar(motivo);
    }
  }
  if (!isnan(efetivo) && abaixo(limite, efetivo)) {
    if (abaixo(efetivo, doConfig)) {
      aviso().printf("Esse limite e de teste (comando lim); sem ele, o efetivo e %.2f A (pelo fundo de escala). "
                     "\"lim padrao\" ou reset volta a ele.\n", efetivo);
    } else {
      aviso().printf("Esse limite e de teste (comando lim); o do config.h e %.2f A. \"lim padrao\" ou reset volta a ele.\n",
                     doConfig);
    }
  }
}

// --------------------------------------------------------------------
//          Limites de teste das protecoes (comando "lim", so na RAM)
// --------------------------------------------------------------------
// Lista os limites de teste: "Iin 0.20 A", "Io 0.20 A" ou "Iin 0.20 A e Io 0.20 A".
static bool textoLimTeste(char *buf, size_t n, bool boost, bool buck) {
  bool iin = (boost && abaixo(limBoostIin(), efBoostIin())) || (buck && abaixo(limBuckIin(), efBuckIin()));
  bool io  = buck && abaixo(limBuckIo(), efBuckIo());
  if (iin && io) snprintf(buf, n, "Iin %.2f A e Io %.2f A", limIinTeste, limBuckIo());
  else if (iin)  snprintf(buf, n, "Iin %.2f A", limIinTeste);
  else if (io)   snprintf(buf, n, "Io %.2f A", limBuckIo());
  else { buf[0] = '\0'; return false; }
  return true;
}

bool controleAvisoLimiteTeste(Print &p, bool soDoModo) {
  char t[40];
  if (!textoLimTeste(t, sizeof(t), !soDoModo || modo == MODO_BOOST, !soDoModo || modo == MODO_BUCK)) return false;
  p.printf(TXT_LIM_TESTE, t);
  return true;
}

// De onde vem o limite sem teste: "config.h: 0.70 A" ou, se o fundo de escala
// baixou o limite, "efetivo: 0.65 A, pelo fundo de escala".
static const char *textoRef(char *buf, size_t n, float efetivo, float doConfig) {
  if (abaixo(efetivo, doConfig)) snprintf(buf, n, "efetivo: %.2f A, pelo fundo de escala", efetivo);
  else snprintf(buf, n, "config.h: %.2f A", doConfig);
  return buf;
}

// "0.70 A", "0.20 A (TESTE; config.h: 0.70 A)" ou, com o fundo de escala abaixo do
// config.h, "0.65 A (pelo fundo de escala; config.h: 0.70 A)" e
// "0.20 A (TESTE; efetivo: 0.65 A, pelo fundo de escala)"
static const char *textoLim(char *buf, size_t n, float ativo, float efetivo, float doConfig) {
  char ref[48];
  if (abaixo(ativo, efetivo)) {
    snprintf(buf, n, "%.2f A (TESTE; %s)", ativo, textoRef(ref, sizeof(ref), efetivo, doConfig));
  } else if (abaixo(efetivo, doConfig)) {
    snprintf(buf, n, "%.2f A (pelo fundo de escala; config.h: %.2f A)", ativo, doConfig);
  } else {
    snprintf(buf, n, "%.2f A", ativo);
  }
  return buf;
}

// true se algum limite efetivo ficou abaixo do config.h (Rsh calibrado grande)
static bool fundoBaixouLimite() {
  return abaixo(efBoostIin(), BOOST_IIN_MAX) || abaixo(efBuckIin(), BUCK_IIN_MAX) || abaixo(efBuckIo(), BUCK_IO_MAX);
}

void controleMostrarLimites() {
  char a[64], b[64], c[64];
  comTempo(con()).println("Limites de protecao dos conversores (testados a cada leitura nova dos INA226, ~50 ms):");
  con().printf("  Boost: Vo %.0f V (fixo) | Iin %s\n", BOOST_VO_MAX,
                textoLim(a, sizeof(a), limBoostIin(), efBoostIin(), BOOST_IIN_MAX));
  con().printf("  Buck:  Iin %s | Io %s\n", textoLim(b, sizeof(b), limBuckIin(), efBuckIin(), BUCK_IIN_MAX),
                textoLim(c, sizeof(c), limBuckIo(), efBuckIo(), BUCK_IO_MAX));
  calMostrarFundoEscala();
  con().printf("Limite efetivo = o menor entre o config.h e 95 %% do fundo de escala (%.2f A no #1, Iin | %.2f A no #2, Io).\n",
                limSensor(0), limSensor(1));
  if (!controleAvisoLimiteTeste(con(), false)) {
    if (fundoBaixouLimite()) con().println("Sem limite de teste. O fundo de escala deixou algum limite abaixo do config.h.");
    else con().println("Todos nos valores do config.h.");
  }
  con().printf("Teste: lim io <A> | lim iin <A> (de %.2f A ate o limite efetivo, com a saida desligada, so na RAM) | lim padrao\n",
                LIM_TESTE_MIN_A);
}

bool controleDefinirLimite(LimiteCorrente qual, float a) {
  if (saidaLigada()) {
    con().println(TXT_LIM_LIGADA);
    return false;
  }
  const bool io = (qual == LIM_IO);
  const float maxConfig = io ? BUCK_IO_MAX : fminf(BOOST_IIN_MAX, BUCK_IIN_MAX);
  const float maxEf = io ? efBuckIo() : fminf(efBoostIin(), efBuckIin());   // o lim aceita ate o efetivo
  a = roundf(a * 100.0f) / 100.0f;   // passos de 0,01 A
  if (!(a >= LIM_TESTE_MIN_A - LIM_FOLGA && a <= maxEf + LIM_FOLGA)) {   // (tambem recusa NaN)
    con().printf("NAO mudei: o limite de %s aceita de %.2f A ate %.2f A (o lim so baixa o limite efetivo; veja \"lim\").\n",
                  io ? "Io" : "Iin", LIM_TESTE_MIN_A, maxEf);
    return false;
  }
  bool teste;
  if (io) {
    limIoTeste = a;
    teste = abaixo(limBuckIo(), efBuckIo());
    if (!teste) limIoTeste = 0.0f;
  } else {
    limIinTeste = a;
    teste = abaixo(limBoostIin(), efBoostIin()) || abaixo(limBuckIin(), efBuckIin());
    if (!teste) limIinTeste = 0.0f;
  }
  const char *quem = io ? "Io do Buck" : "Iin do Boost e do Buck";
  char ref[48];
  if (teste) {
    comTempo(con()).printf("Limite de %s = %.2f A (TESTE, so na RAM; %s). \"lim padrao\" ou reset volta ao config.h.\n",
                  quem, a, textoRef(ref, sizeof(ref), maxEf, maxConfig));
  } else if (abaixo(maxEf, maxConfig)) {
    comTempo(con()).printf("Limite de %s = %.2f A (sem teste: o efetivo, pelo fundo de escala; config.h: %.2f A).\n",
                  quem, a, maxConfig);
  } else {
    comTempo(con()).printf("Limite de %s = %.2f A (valor do config.h).\n", quem, a);
  }
  return true;
}

bool controleLimitesPadrao() {
  if (saidaLigada()) {
    con().println(TXT_LIM_LIGADA);
    return false;
  }
  limIinTeste = 0.0f;
  limIoTeste = 0.0f;
  comTempo(con()).printf("Limites de volta ao config.h: Boost Vo %.0f V e Iin %.2f A | Buck Iin %.2f A e Io %.2f A.\n",
                BOOST_VO_MAX, efBoostIin(), efBuckIin(), efBuckIo());
  if (fundoBaixouLimite()) {
    con().println("Obs.: com o Rsh calibrado, algum limite ficou abaixo do config.h (95 % do fundo de escala): veja \"lim\".");
  }
  return true;
}

// --------------------------------------------------------------------
void controleIniciar() {
  pwmDesligar();
  scrDesligar();
  modo = MODO_RET_NC;
  pwmSelecionar(CONV_BOOST);
  scrDefinirModo(2);
  scrDefinirAlfa(180.0f, false);
  limIinTeste = 0.0f;   // limites de teste do "lim": reset volta ao config.h
  limIoTeste = 0.0f;
#if PAINEL_MANUAL
  peloPot = true;
#endif
}

void controleAtualizar() {
  uint32_t agora = millis();

  // 1) Potenciometros -> valores pedidos (mesmo com a saida desligada, para a tela).
  //    Com PAINEL_MANUAL 0 o alvo vem so dos comandos d e a.
#if PAINEL_MANUAL
  if (peloPot) {
    if (modoConversor(modo)) pwmDefinirAlvo(potD(pwmDMax()));
    else if (modoScr(modo)) scrDefinirAlfa(potAlfa(), false);
  }
#endif

  // 2) Rampa de D
  if (pwmAtualizar() && !peloPot) comTempo(aviso()).printf("D chegou ao alvo: D = %.3f\n", pwmDAtual());

  // 3) Vigia da rede (ZC/POL). As mensagens de rotina so nos modos de retificador.
  scrVerificar(!modoConversor(modo));
  const char *m = scrPegarMotivo();
  if (m != nullptr) {
    ultimaProtecaoMs = millis();
    snprintf(ultimaProtecao, sizeof(ultimaProtecao), "%s", m);
    protecaoAtual = true;
    telaMensagem("DESLIGOU:", m, nullptr, 5000);
  }

  // 4) Protecoes dos conversores
  verificarProtecoes();

  // 5) LEDs
  if (agora - ultimoLedMs >= 50) {
    ultimoLedMs = agora;
    ledsEscrever(saidaLigada(), scrResumo().sincronizado);
  }

  // 6) Calibracao nova: grava na NVS so com a saida desligada (gravar na
  //    flash pausa as interrupcoes por alguns ms e atrasaria o disparo).
  if (calPendente() && !saidaLigada()) calGravar();
}
