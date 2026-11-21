/*
  painel.h - LEDs e (opcional) botoes e potenciometros da bancada.

  LEDs (sempre):
    LED "saida ligada": aceso com o PWM ou o disparo ligado
    LED "sincronizado": aceso com a rede sincronizada

  Botoes e potenciometros: so com PAINEL_MANUAL 1 no config.h. Com 0 (padrao)
  o comando e so pela pagina web e pela serial, e este modulo nao le nenhum pino.

  Com PAINEL_MANUAL 1:
  Botoes (entre o pino e o GND, pull-up interno, apertado = LOW):
    MODO  (GPIO5): toque curto = proximo modo; segurar 0,8 s = proxima pagina da tela
    LIGA  (GPIO6): liga/desliga a saida (com a trava dos potenciometros)
    ZERO  (GPIO7): zero do ACS712 (so com a saida desligada e sem corrente)
  Potenciometros (extremos em 3,3 V e GND, cursor no pino):
    Pot 1 (GPIO1): D, de 0 (inicio) ate DMAX (fim)
    Pot 2 (GPIO2): alfa, de 180 graus (inicio) ate 0 grau (fim)
    Pot 3 (GPIO4): pagina da tela (3 faixas)
*/
#pragma once
#include <Arduino.h>
#include "config.h"

void painelIniciar();
void painelAtualizar();          // chamar sempre no loop (com PAINEL_MANUAL 0 nao faz nada)
void ledsEscrever(bool saida, bool sinc);

#if PAINEL_MANUAL
const uint8_t POT_D    = 0;
const uint8_t POT_ALFA = 1;
const uint8_t POT_3    = 2;

float potPosicao(uint8_t k);     // 0..1, filtrada
bool potNoZero(uint8_t k);       // no inicio do curso (posicao segura)
float potD(float dmax);          // D pedido pelo Pot 1 (passos de 0,005, com histerese)
float potAlfa();                 // alfa pedido pelo Pot 2 (passos de 1 grau, com histerese)
#endif
