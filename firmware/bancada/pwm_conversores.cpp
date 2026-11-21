/*
  pwm_conversores.cpp - PWM do Boost e do Buck (periferico LEDC do ESP32).

  O PWM e gerado por hardware: depois de configurado, ele continua saindo
  certinho mesmo que o loop demore (I2C, tela, serial).

  Boost: GPIO15 -> TC4427 IN A. "Desligado" = D = 0 (MOSFET sempre desligado).
  Buck:  GPIO16 -> IRS2104 IN e GPIO17 -> IRS2104 SD (barrado).
         "Desligado" = SD em LOW (Q1 e Q2 desligados) e D = 0.
         Ao ligar: D = 0 primeiro (Q2 ligado, o capacitor de bootstrap carrega)
         e so depois SD em HIGH.
*/
#include <Arduino.h>
#include "config.h"
#include "pwm_conversores.h"
#include "comandos.h"

static const int      PINO_PWM[2] = { PINO_BOOST_PWM, PINO_BUCK_PWM };
static const float    DMAX[2]     = { BOOST_DMAX, BUCK_DMAX };
static const uint32_t FMIN[2]     = { BOOST_F_MIN, BUCK_F_MIN };
static const uint32_t FMAX[2]     = { BOOST_F_MAX, BUCK_F_MAX };

static Conversor ativo   = CONV_BOOST;
static bool      ligado  = false;
static float     dAlvo   = 0.0f;
static float     dAtual  = 0.0f;
static uint32_t  freqHz[2] = { BOOST_F_PADRAO, BUCK_F_PADRAO };
static uint8_t   bits[2]   = { 9, 10 };
static bool      anexado[2] = { false, false };
static uint32_t  ultimoPasso = 0;

// Maior resolucao possivel para a frequencia (o LEDC usa clock de 80 MHz):
// 100 kHz -> 9 bits (512 passos); 50 kHz -> 10 bits (1024 passos).
static uint8_t bitsPara(uint32_t f) {
  uint8_t b = 1;
  while (b < 14 && (80000000UL >> (b + 1)) >= f) b++;
  return b;
}

static void escreverDuty(uint8_t c, float d) {
  if (!anexado[c]) return;
  uint32_t contagem = (uint32_t)(d * (float)(1UL << bits[c]) + 0.5f);
  ledcWrite(PINO_PWM[c], contagem);
}

static bool configurarPWM(uint8_t c) {
  bits[c] = bitsPara(freqHz[c]);
  if (anexado[c]) ledcDetach(PINO_PWM[c]);
  anexado[c] = ledcAttach(PINO_PWM[c], freqHz[c], bits[c]);
  escreverDuty(c, 0.0f);
  return anexado[c];
}

void pwmIniciar() {
  // O nucleo 3.x usa o cristal de 40 MHz no LEDC da S3, e 100 kHz x 9 bits (51,2 MHz) nao cabe
  // ("ERRO ao configurar o PWM"). Com o APB de 80 MHz cabe. Tem de vir antes do ledcAttach.
  ledcSetClockSource(LEDC_USE_APB_CLK);
  // Primeiro o driver do Buck desligado, depois os PWMs em 0.
  pinMode(PINO_BUCK_SD, OUTPUT);
  digitalWrite(PINO_BUCK_SD, LOW);
  pinMode(PINO_BOOST_PWM, OUTPUT);
  digitalWrite(PINO_BOOST_PWM, LOW);
  pinMode(PINO_BUCK_PWM, OUTPUT);
  digitalWrite(PINO_BUCK_PWM, LOW);
  if (!configurarPWM(CONV_BOOST)) con().println("ERRO ao configurar o PWM do Boost (" TXT_PWM_BOOST ")!");
  if (!configurarPWM(CONV_BUCK)) con().println("ERRO ao configurar o PWM do Buck (" TXT_PWM_BUCK ")!");
  ligado = false;
  dAlvo = 0.0f;
  dAtual = 0.0f;
}

void pwmDesligar() {
  digitalWrite(PINO_BUCK_SD, LOW);  // primeiro o driver do Buck: Q1 e Q2 desligam na hora
  escreverDuty(CONV_BOOST, 0.0f);
  escreverDuty(CONV_BUCK, 0.0f);
  ligado = false;
  dAtual = 0.0f;
}

void pwmSelecionar(Conversor c) {
  pwmDesligar();
  ativo = c;
  dAlvo = 0.0f;
}

void pwmLigar() {
  // Sem corrente no indutor (estava desligado): pode comecar em D = 0 na hora.
  dAtual = 0.0f;
  escreverDuty(ativo, 0.0f);
  if (ativo == CONV_BUCK) digitalWrite(PINO_BUCK_SD, HIGH);  // driver ligado com D = 0: Q2 ligado
  ligado = true;
  ultimoPasso = millis();
}

void pwmDefinirAlvo(float d) {
  if (d < 0.0f) d = 0.0f;
  if (d > DMAX[ativo]) d = DMAX[ativo];
  dAlvo = d;
}

bool pwmDefinirFrequencia(uint32_t hz) {
  if (hz < FMIN[ativo]) hz = FMIN[ativo];
  if (hz > FMAX[ativo]) hz = FMAX[ativo];
  pwmDesligar();
  dAlvo = 0.0f;
  freqHz[ativo] = hz;
  return configurarPWM(ativo);
}

bool pwmAtualizar() {
  uint32_t agora = millis();
  if (agora - ultimoPasso < PERIODO_RAMPA_MS) return false;
  ultimoPasso = agora;
  if (!ligado) return false;
  float antes = dAtual;
  if (dAtual < dAlvo) dAtual = fminf(dAlvo, dAtual + PASSO_RAMPA_D);
  else if (dAtual > dAlvo) dAtual = fmaxf(dAlvo, dAtual - PASSO_RAMPA_D);
  if (dAtual != antes) {
    escreverDuty(ativo, dAtual);
    return dAtual == dAlvo;
  }
  return false;
}

bool      pwmLigado()      { return ligado; }
Conversor pwmConversor()   { return ativo; }
float     pwmDAtual()      { return dAtual; }
float     pwmDAlvo()       { return dAlvo; }
float     pwmDMax()        { return DMAX[ativo]; }
uint32_t  pwmFrequencia()  { return freqHz[ativo]; }
uint32_t  pwmFreqMin()     { return FMIN[ativo]; }
uint32_t  pwmFreqMax()     { return FMAX[ativo]; }
uint8_t   pwmBits()        { return bits[ativo]; }
