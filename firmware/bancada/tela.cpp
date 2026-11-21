/*
  tela.cpp - OLED 128x64 com a biblioteca U8g2 (buffer inteiro na memoria).

  Para nao travar o loop: o quadro e desenhado na memoria (rapido) e enviado
  ao display em 8 faixas de 128 x 8 pixels, uma por volta do loop (~3 ms
  cada, a 400 kHz). Enviar a tela inteira de uma vez levaria ~25 ms.
*/
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>
#include <string.h>
#include "config.h"
#include "tela.h"
#include "controle.h"
#include "pwm_conversores.h"
#include "disparo_scr.h"
#include "medicao.h"
#include "painel.h"
#include "comandos.h"
#include "web.h"

#if OLED_SH1106
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

static bool     presente       = false;
static uint8_t  pagina         = 0;
static bool     paginaAuto     = true;   // troca sozinha a cada TELA_TROCA_MS (o Pot 3 e o MODO fixam)
static uint32_t ultimaTrocaMs  = 0;
static uint8_t  faixaEnvio     = 8;   // 0..7 = enviando essa faixa; 8 = nada a enviar
static uint32_t ultimoQuadroMs = 0;
static uint32_t ultimaProcuraMs = 0;

static char     msg[3][20];
static uint8_t  msgLinhas = 0;
static uint32_t msgAteMs  = 0;

// --------------------------------------------------------------------
void telaDetectar(bool imprimir) {
  presente = i2cResponde(END_OLED);
  if (presente) {
    u8g2.setI2CAddress(END_OLED * 2);  // a U8g2 usa o endereco de 8 bits
    u8g2.setBusClock(I2C_FREQ_HZ);
    u8g2.begin();                      // inicia e limpa a tela (~25 ms, so aqui)
    faixaEnvio = 8;
    ultimoQuadroMs = 0;
  }
  if (imprimir) con().printf("OLED (0x3C): %s\n", presente ? "OK" : "AUSENTE");
}

void telaIniciar() {
  telaDetectar(true);
}

bool telaPresente() {
  return presente;
}

void telaProximaPagina() {
  pagina = (uint8_t)((pagina + 1) % TELA_NUM_PAGINAS);
  paginaAuto = false;
  ultimoQuadroMs = 0;  // redesenha ja
}

void telaDefinirPagina(uint8_t p) {
  if (p >= TELA_NUM_PAGINAS) p = TELA_NUM_PAGINAS - 1;
  pagina = p;
  paginaAuto = false;
  ultimoQuadroMs = 0;
}

void telaAuto() {
  paginaAuto = true;
  ultimaTrocaMs = millis();
}

bool telaEhAuto() {
  return paginaAuto;
}

uint8_t telaPagina() {
  return pagina;
}

void telaMensagem(const char *l1, const char *l2, const char *l3, uint32_t ms) {
  const char *l[3] = { l1, l2, l3 };
  msgLinhas = 0;
  for (uint8_t i = 0; i < 3; i++) {
    msg[i][0] = '\0';
    if (l[i] != nullptr) {
      strncpy(msg[i], l[i], sizeof(msg[i]) - 1);
      msg[i][sizeof(msg[i]) - 1] = '\0';
      msgLinhas = (uint8_t)(i + 1);
    }
  }
  msgAteMs = millis() + ms;
  ultimoQuadroMs = 0;
}

// --------------------------------------------------------------------
//                         Desenho de cada parte
// --------------------------------------------------------------------
static void valor(char *buf, size_t n, const char *rotulo, float v, int largura, int dec, const char *unidade) {
  if (isnan(v)) snprintf(buf, n, "%s  ---", rotulo);
  else snprintf(buf, n, "%s%*.*f %s", rotulo, largura, dec, v, unidade);
}

static void cabecalho() {
  Modo m = modoAtual();
  char dir[24];
  bool lig = saidaLigada();
  const char *marca = controlePotForaDoZero() ? "!" : "";
  if (modoConversor(m)) {
    float d = lig ? pwmDAtual() : controleAlvo();
    snprintf(dir, sizeof(dir), "%s D=%.3f%s", lig ? "LIG" : "DESL", d, marca);
  } else if (modoScr(m)) {
    ScrResumo s = scrResumo();
    float a = s.armado ? s.alfaAtual : s.alfaAlvo;
    snprintf(dir, sizeof(dir), "%s a=%.0f%s", lig ? "LIG" : "DESL", a, marca);
  } else {
    snprintf(dir, sizeof(dir), "medicao");
  }
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, 128, 11);
  u8g2.setDrawColor(0);
  u8g2.drawStr(2, 9, nomeModoTela(m));
  u8g2.drawStr((u8g2_uint_t)(126 - u8g2.getStrWidth(dir)), 9, dir);
  u8g2.setDrawColor(1);
}

// 3 linhas em letra grande (9x15)
static void tresLinhasGrandes(const char *a, const char *b, const char *c) {
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(0, 26, a);
  u8g2.drawStr(0, 43, b);
  u8g2.drawStr(0, 60, c);
}

// 4 linhas em letra media (7x13)
static void quatroLinhas(const char *a, const char *b, const char *c, const char *d) {
  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.drawStr(0, 24, a);
  u8g2.drawStr(0, 37, b);
  u8g2.drawStr(0, 50, c);
  u8g2.drawStr(0, 63, d);
}

static void textoRede(char *buf, size_t n) {
  ScrResumo s = scrResumo();
  if (!s.zcPresente) snprintf(buf, n, "sem rede (ZC)");
  else if (!s.sincronizado) snprintf(buf, n, "ZC sem sinc.");
  else snprintf(buf, n, "%.2f Hz SINC", s.freqRede);
}

static void paginaConversor(uint8_t p, const Resultados &r) {
  char a[24], b[24], c[24], d[24];
  if (p == 0) {
    valor(a, sizeof(a), "Vo ", r.vo, 7, 2, "V");
    valor(b, sizeof(b), "Io ", r.io, 7, 3, "A");
    valor(c, sizeof(c), "eta", r.eta, 7, 1, "%");
    tresLinhasGrandes(a, b, c);
  } else {
    valor(a, sizeof(a), "Vin ", r.vin, 8, 3, "V");
    valor(b, sizeof(b), "Iin ", r.iin, 8, 4, "A");
    valor(c, sizeof(c), "Pin ", r.pin, 8, 3, "W");
    valor(d, sizeof(d), "Pout", r.pout, 8, 3, "W");
    quatroLinhas(a, b, c, d);
  }
}

static void paginaRetificador(uint8_t p, const Resultados &r) {
  char a[24], b[24], c[24], d[24];
  if (!adsPresente()) {
    textoRede(c, sizeof(c));
    tresLinhasGrandes("ADS1115", "ausente", c);
    return;
  }
  if (p == 0) {
    valor(a, sizeof(a), "Vmed", r.voMed, 7, 2, "V");
    valor(b, sizeof(b), "Imed", r.ioMed, 7, 3, "A");
    textoRede(c, sizeof(c));
    tresLinhasGrandes(a, b, c);
  } else {
    valor(a, sizeof(a), "Vrms", r.voRms, 8, 3, "V");
    valor(b, sizeof(b), "Irms", r.ioRms, 8, 4, "A");
    valor(c, sizeof(c), "eta ", r.etaRet, 8, 1, "%");
    valor(d, sizeof(d), "V/R ", r.ioRMed, 8, 4, "A");
    quatroLinhas(a, b, c, d);
  }
}

static void paginaEstado() {
  char l[5][28];
  Modo m = modoAtual();
  ScrResumo s = scrResumo();
#if PAINEL_MANUAL
  snprintf(l[0], sizeof(l[0]), "%s P1:%.0f%% P2:%.0f%%", controlePeloPot() ? "POT" : "SER",
           100.0f * potPosicao(POT_D), 100.0f * potPosicao(POT_ALFA));
#else
  const char *quem = webNoControle() ? "WEB" : "serial";
  snprintf(l[0], sizeof(l[0]), "Ctl %s tela %s", quem, paginaAuto ? "auto" : "fixa");
#endif
  snprintf(l[1], sizeof(l[1]), "IN1 %s IN2 %s ADS %s", inaPresente(0) ? "ok" : "--",
           inaPresente(1) ? "ok" : "--", adsPresente() ? "ok" : "--");
  if (!s.zcPresente) snprintf(l[2], sizeof(l[2]), "Rede: sem ZC");
  else snprintf(l[2], sizeof(l[2]), "Rede %.2fHz %s %s", s.freqRede, s.sincronizado ? "SINC" : "----",
                s.polOk ? "POL" : "---");
  if (modoConversor(m)) {
    snprintf(l[3], sizeof(l[3]), "f %lu Hz %u bits", (unsigned long)pwmFrequencia(), (unsigned)pwmBits());
  } else {
    snprintf(l[3], sizeof(l[3]), "pulso %luus R %.1f", (unsigned long)s.pulso_us, cal.rCarga);
  }
  const char *prot = controleProtecaoAtual();   // some ao religar ou pegar o controle
  if (prot[0] != '\0') snprintf(l[4], sizeof(l[4]), "!%s", prot);
  else snprintf(l[4], sizeof(l[4]), "ACS0 %.3fV", cal.acsZero);
  u8g2.setFont(u8g2_font_6x10_tr);
  for (uint8_t i = 0; i < 5; i++) u8g2.drawStr(0, (u8g2_uint_t)(21 + 10 * i), l[i]);
}

static void corpoMensagem() {
  u8g2.drawFrame(0, 13, 128, 51);
  u8g2.setFont(u8g2_font_7x13B_tr);
  for (uint8_t i = 0; i < msgLinhas; i++) u8g2.drawStr(4, (u8g2_uint_t)(28 + 14 * i), msg[i]);
}

static void desenhar() {
  u8g2.clearBuffer();
  cabecalho();
  if ((int32_t)(msgAteMs - millis()) > 0) {
    corpoMensagem();
    return;
  }
  const Resultados &r = medicaoResultados();
  Modo m = modoAtual();
  if (pagina == 2) paginaEstado();
  else if (modoConversor(m)) paginaConversor(pagina, r);
  else paginaRetificador(pagina, r);
}

// --------------------------------------------------------------------
void telaAtualizar() {
  uint32_t agora = millis();
  if (!presente) {
    // Procura o display de novo de vez em quando (fio solto e recolocado).
    if (agora - ultimaProcuraMs >= REDETECTAR_MS) {
      ultimaProcuraMs = agora;
      if (i2cResponde(END_OLED)) {
        telaDetectar(false);
        comTempo(aviso()).println("OLED (0x3C) encontrado.");
      }
    }
    return;
  }
  if (paginaAuto && agora - ultimaTrocaMs >= TELA_TROCA_MS) {
    // Troca automatica de pagina (sem o Pot 3 e o botao MODO).
    ultimaTrocaMs = agora;
    pagina = (uint8_t)((pagina + 1) % TELA_NUM_PAGINAS);
    ultimoQuadroMs = 0;  // desenha a pagina nova assim que a faixa atual terminar
  }
  if (faixaEnvio < 8) {
    u8g2.updateDisplayArea(0, faixaEnvio, 16, 1);  // uma faixa de 8 linhas de pixels
    faixaEnvio++;
    return;
  }
  if (ultimoQuadroMs != 0 && agora - ultimoQuadroMs < TELA_PERIODO_MS) return;
  ultimoQuadroMs = agora;
  if (!i2cResponde(END_OLED)) {
    presente = false;
    ultimaProcuraMs = agora;
    comTempo(aviso()).println("ATENCAO: OLED (0x3C) parou de responder.");
    return;
  }
  desenhar();
  faixaEnvio = 0;
}
