/*
  medicao.h - Medicoes pelo I2C, com drivers minimos (por registrador, sem
  biblioteca) para:
    - INA226 #1 (0x40): entrada do conversor -> Vin e Iin
    - INA226 #2 (0x41): saida do conversor   -> Vo e Io
    - ADS1115   (0x48): retificadores -> A0 = Vo (divisor), A1 = Io (ACS712)
  e as calibracoes gravadas na memoria do ESP32 (NVS).

  Todos os resultados sao MEDIAS DE 1 s (janela). Valor indisponivel = NAN.
*/
#pragma once
#include <Arduino.h>

struct Calibracao {
  float rsh1;     // Ohm, shunt do INA226 #1 (entrada)
  float rsh2;     // Ohm, shunt do INA226 #2 (saida)
  float div;      // razao do divisor do A0: Vo = V_A0 x div
  float acsSens;  // V/A do ACS712 (negativo inverte o sentido)
  float acsZero;  // V na saida do ACS712 com corrente zero
  float rCarga;   // Ohm, carga do retificador (Io = Vo/R, conferencia)
};
extern Calibracao cal;

enum ItemCal : uint8_t { CAL_RSH1, CAL_RSH2, CAL_DIV, CAL_ACS, CAL_ACS_ZERO, CAL_RCARGA };

// Ultima leitura de um INA226 (a cada ~50 ms; usada nas protecoes)
struct LeituraIna {
  bool  ok;      // false se o INA226 nao responde
  float v;       // V, tensao do barramento (pino VBUS)
  float i;       // A, corrente = Vshunt / Rshunt
  float vShunt;  // V, tensao no shunt
  bool  saturado; // |registrador do shunt| >= INA_SHUNT_SAT_CRU: no fundo de escala
};

// Resultados de uma janela de 1 s
struct Resultados {
  uint32_t id;        // aumenta 1 a cada janela
  // INA226 (conversores)
  float vin, iin, pin;
  float vo, io, pout;
  float eta;          // %, Pout/Pin
  float vShunt1, vShunt2;
  // ADS1115 (retificadores)
  float voMed, voRms, ioMed, ioRms;
  float vA0, vA1;     // V, medias nos pinos A0 e A1 (para calibrar)
  float vA0max;       // V, maior amostra no A0 (perto de 3,3 V = limite)
  float pdc, pac;     // W: Vo_med x Io_med e Vo_rms x Io_rms
  float etaRet;       // %, Pdc/Pac
  float ioRMed, ioRRms;  // A, Io calculada por Vo/R
  uint16_t nA0, nA1;  // amostras na janela
  bool porFase;       // true = tempo equivalente (amostras ordenadas pela fase da rede)
  float lacunaGraus;  // maior buraco entre fases vizinhas, em graus (so com porFase)
};

// ---- Calibracao (NVS) ----
void calPadrao();                       // volta aos valores do config.h (e marca para gravar)
void calCarregar();                     // le da NVS (no setup)
bool calDefinir(ItemCal item, float v); // false = valor fora da faixa aceita
void calMostrar();                      // inclui a linha do fundo de escala dos INA226
void calMostrarFundoEscala();           // "Fundo de escala dos INA226 (81.92 mV / Rsh): ..."
float medicaoFundoEscala(uint8_t k);    // A: 81,92 mV / Rsh calibrado (k = 0: #1, k = 1: #2)
bool calPendente();
void calGravar();                       // grava na NVS (so com a saida desligada: ver controle.cpp)

// ---- Medicao ----
void medicaoIniciar();                  // Wire.begin + procura os dispositivos
void medicaoDetectar(bool imprimir);    // procura de novo (comando "i2c")
void medicaoAtualizar();                // chamar sempre no loop
bool i2cResponde(uint8_t endereco);
bool inaPresente(uint8_t k);            // k = 0 (entrada) ou 1 (saida)
bool adsPresente();
const LeituraIna &medicaoIna(uint8_t k);
bool medicaoInaNova();                  // true uma vez a cada rodada de leitura dos INA226 (uso do controle.cpp)
uint32_t medicaoInaContador();          // aumenta 1 a cada rodada (para quem so quer saber se mudou)
const Resultados &medicaoResultados();
void medicaoPedirZero();                // zero do ACS712 na proxima janela completa
bool medicaoZeroEmAndamento();
