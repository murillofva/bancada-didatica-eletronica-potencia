/*
  pwm_conversores.h - PWM do Boost (GPIO15 -> TC4427) e do Buck sincrono
  (GPIO16 -> IN do IRS2104, GPIO17 -> SD barrado do IRS2104).

  PWM por hardware (LEDC), rampa de D (0,005 a cada 10 ms), limite DMAX.
  So um conversor fica "ativo" por vez (o do modo escolhido).
*/
#pragma once
#include <Arduino.h>

enum Conversor : uint8_t { CONV_BOOST = 0, CONV_BUCK = 1 };

void pwmIniciar();                       // no setup: tudo desligado (SD em LOW, PWM em 0)
void pwmSelecionar(Conversor c);         // desliga tudo e escolhe o conversor ativo
void pwmDesligar();                      // desliga na hora: SD em LOW, D = 0 nos dois pinos
void pwmLigar();                         // liga o ativo com D = 0 (Buck: SD em HIGH); D sobe em rampa ate o alvo
void pwmDefinirAlvo(float d);            // alvo de D (limitado a 0..DMAX)
bool pwmDefinirFrequencia(uint32_t hz);  // muda a frequencia do ativo (desliga antes); false = erro
bool pwmAtualizar();                     // rampa; chamar sempre. true = D acabou de chegar ao alvo

bool      pwmLigado();
Conversor pwmConversor();
float     pwmDAtual();
float     pwmDAlvo();
float     pwmDMax();
uint32_t  pwmFrequencia();
uint32_t  pwmFreqMin();
uint32_t  pwmFreqMax();
uint8_t   pwmBits();
