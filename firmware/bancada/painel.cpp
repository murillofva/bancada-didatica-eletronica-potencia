/*
  painel.cpp - LEDs e, com PAINEL_MANUAL 1, botoes (com debounce) e
  potenciometros (filtro + histerese).
*/
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "painel.h"
#include "controle.h"
#include "tela.h"

// ----------------------------- LEDs -----------------------------
void ledsEscrever(bool saida, bool sinc) {
  digitalWrite(PINO_LED_SAIDA, saida ? HIGH : LOW);
  digitalWrite(PINO_LED_SINC, sinc ? HIGH : LOW);
}

#if !PAINEL_MANUAL
// Sem botoes e sem potenciometros: so os LEDs. Nenhum pino e lido aqui.
void painelIniciar() {
  pinMode(PINO_LED_SAIDA, OUTPUT);
  pinMode(PINO_LED_SINC, OUTPUT);
  ledsEscrever(false, false);
}

void painelAtualizar() {
}

#else  // PAINEL_MANUAL 1: botoes e potenciometros

// ---------------------------- Botoes ----------------------------
struct Botao {
  int      pino;
  bool     apertado;   // estado ja filtrado (debounce)
  bool     leitura;    // ultima leitura crua
  uint32_t mudouMs;    // quando a leitura crua mudou
  uint32_t apertouMs;  // quando o botao foi apertado
  bool     longo;      // o "toque longo" ja foi tratado
};

static Botao botoes[3] = {
  { PINO_BOTAO_MODO, false, false, 0, 0, false },
  { PINO_BOTAO_LIGA, false, false, 0, 0, false },
  { PINO_BOTAO_ZERO, false, false, 0, 0, false },
};

static const uint8_t EV_NADA    = 0;
static const uint8_t EV_APERTOU = 1;
static const uint8_t EV_SOLTOU  = 2;
static const uint8_t EV_LONGO   = 3;

static uint8_t lerBotao(Botao &b, uint32_t agora) {
  bool leitura = (digitalRead(b.pino) == LOW);
  if (leitura != b.leitura) {
    b.leitura = leitura;
    b.mudouMs = agora;
  }
  if (leitura != b.apertado && agora - b.mudouMs >= BOTAO_DEBOUNCE_MS) {
    b.apertado = leitura;
    if (leitura) {
      b.apertouMs = agora;
      b.longo = false;
      return EV_APERTOU;
    }
    return EV_SOLTOU;
  }
  if (b.apertado && !b.longo && agora - b.apertouMs >= BOTAO_LONGO_MS) {
    b.longo = true;
    return EV_LONGO;
  }
  return EV_NADA;
}

// ------------------------ Potenciometros ------------------------
static const int PINO_POT[3] = { PINO_POT_D, PINO_POT_ALFA, PINO_POT_3 };
static float    filtro[3]   = { 0.0f, 0.0f, 0.0f };  // leitura filtrada do ADC (0..4095)
static uint32_t ultimoPotMs = 0;
static float    dQuant      = 0.0f;
static float    alfaQuant   = 180.0f;
static int8_t   zonaPot3    = -1;

float potPosicao(uint8_t k) {
  if (k > 2) return 0.0f;
  float p = (filtro[k] - POT_CRU_MIN) / (POT_CRU_MAX - POT_CRU_MIN);
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  return p;
}

bool potNoZero(uint8_t k) {
  return potPosicao(k) <= POT_ZONA_SEGURA;
}

// Quantiza com histerese: so muda de passo quando o valor andou 3/4 de passo.
// Assim o ruido do ADC nao fica "piscando" o D ou o alfa.
static float quantizar(float x, float &guardado, float passo) {
  if (fabsf(x - guardado) > 0.75f * passo) guardado = roundf(x / passo) * passo;
  return guardado;
}

float potD(float dmax) {
  float d = quantizar(potPosicao(POT_D) * dmax, dQuant, POT_PASSO_D);
  if (d > dmax) d = dmax;
  if (d < 0.0f) d = 0.0f;
  return d;
}

float potAlfa() {
  // Girar para o fim do curso = MAIS potencia = alfa MENOR.
  float a = quantizar(180.0f * (1.0f - potPosicao(POT_ALFA)), alfaQuant, POT_PASSO_ALFA);
  if (a > 180.0f) a = 180.0f;
  if (a < 0.0f) a = 0.0f;
  return a;
}

static void lerPots() {
  for (uint8_t k = 0; k < 3; k++) {
    float cru = (float)analogRead(PINO_POT[k]);
    filtro[k] += POT_FILTRO * (cru - filtro[k]);
  }
}

static void pot3Pagina(bool inicio) {
  if (!POT3_MUDA_PAGINA) return;
  float p = potPosicao(POT_3);
  int8_t zona = (p < 1.0f / 3.0f) ? 0 : ((p < 2.0f / 3.0f) ? 1 : 2);
  if (inicio || zonaPot3 < 0) {
    zonaPot3 = zona;
    telaDefinirPagina((uint8_t)zona);
    return;
  }
  if (zona == zonaPot3) return;
  // Histerese: precisa passar 4 % alem da divisa da faixa atual.
  float baixo = zonaPot3 / 3.0f - 0.04f;
  float alto = (zonaPot3 + 1) / 3.0f + 0.04f;
  if (p < baixo || p > alto) {
    zonaPot3 = zona;
    telaDefinirPagina((uint8_t)zona);
  }
}

// --------------------------- Publicas ---------------------------
void painelIniciar() {
  pinMode(PINO_LED_SAIDA, OUTPUT);
  pinMode(PINO_LED_SINC, OUTPUT);
  ledsEscrever(false, false);

  uint32_t agora = millis();
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(botoes[i].pino, INPUT_PULLUP);
  }
  delay(5);  // pull-up interno estabilizar (so no inicio)
  for (uint8_t i = 0; i < 3; i++) {
    bool ap = (digitalRead(botoes[i].pino) == LOW);
    botoes[i].apertado = ap;
    botoes[i].leitura = ap;
    botoes[i].mudouMs = agora;
    botoes[i].apertouMs = agora;
    botoes[i].longo = ap;  // botao segurado no boot nao vale como toque
  }

  analogReadResolution(12);
  for (uint8_t k = 0; k < 3; k++) filtro[k] = (float)analogRead(PINO_POT[k]);
  dQuant = 0.0f;
  alfaQuant = 180.0f;
  ultimoPotMs = agora;
  pot3Pagina(true);
}

void painelAtualizar() {
  uint32_t agora = millis();

  uint8_t e = lerBotao(botoes[0], agora);       // MODO
  if (e == EV_SOLTOU && !botoes[0].longo) controleProximoModo();
  else if (e == EV_LONGO) telaProximaPagina();

  if (lerBotao(botoes[1], agora) == EV_APERTOU) controleAlternar();  // LIGA-DESLIGA
  if (lerBotao(botoes[2], agora) == EV_APERTOU) controleZero();      // ZERO

  if (agora - ultimoPotMs >= POT_PERIODO_MS) {
    ultimoPotMs = agora;
    lerPots();
    pot3Pagina(false);
  }
}

#endif  // PAINEL_MANUAL
