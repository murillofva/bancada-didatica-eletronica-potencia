/*
  disparo_scr.h - Disparo dos SCRs sincronizado com a rede: interrupcoes, timers,
  deteccao de sincronismo, sinal POL e vigia de 50 ms (ver disparo_scr.cpp).

  Modos do motor: 1 = meia onda controlada (so GATE_A)
                  2 = ponte semicontrolada (GATE_A no semiciclo positivo,
                      GATE_B no negativo)
*/
#pragma once
#include <Arduino.h>

// Resumo do estado, para a tela, o log e o "?".
struct ScrResumo {
  bool     armado;        // disparo ligado
  bool     zcPresente;    // chegam pulsos validos de ZC no GPIO41
  bool     sincronizado;  // rede medida e estavel (45 a 65 Hz)
  bool     polOk;         // sinal POL (GPIO42) alternando
  uint8_t  modo;          // 1 = meia onda, 2 = ponte
  float    alfaAlvo;      // graus
  float    alfaAtual;     // graus (anda em rampa ate o alvo; 180 = sem disparo)
  float    freqRede;      // Hz (0 se nao estiver sincronizado)
  uint32_t pulso_us;      // largura do pulso de gate
  // Para a pagina: os mesmos valores do "?".
  float    semi_us;       // meio periodo T/2 filtrado (us)
  float    w_us;          // largura W filtrada do pulso de ZC (us)
  uint32_t dispA, dispB;  // semiciclos em que o GATE_A / GATE_B deu pulso (zeram ao ligar o disparo)
};

// Fase da rede num instante, para a medicao em "tempo equivalente".
struct FaseRede {
  bool     valida;   // ZC presente e sincronizado
  uint32_t t_us;     // us desde a ultima subida (aceita) do pulso de ZC
  uint32_t semi_us;  // meio periodo T/2 filtrado
  uint32_t w_us;     // largura W filtrada do pulso de ZC
  uint32_t meio;     // contador do semiciclo que comecou na ultima subida (so a paridade importa)
};

void scrIniciar();                    // timers + interrupcoes (no setup); gates em LOW
bool scrDisponivel();                 // false se os timers nao puderam ser criados
void scrDesligar();                   // desliga o disparo na hora (gates em LOW)
bool scrLigar();                      // liga (so com a rede sincronizada); avisa na serial
void scrDefinirModo(uint8_t m);       // 1 ou 2; desliga o disparo
void scrDefinirAlfa(float graus, bool avisarFimRampa);
void scrDefinirPulso(uint32_t us);    // largura do pulso de gate (limitada a 100..2000 us)
void scrDefinirRampa(float grausPorSemiciclo);
float scrRampa();                     // graus por semiciclo
void scrVerificar(bool mensagens);    // chamar sempre no loop: vigia do ZC/sincronismo
void scrMostrarEstado();              // parte do "?" nos modos com SCR
ScrResumo scrResumo();
FaseRede scrFase();
// Vo teorica (carga R, ideal, Vm = VM_PICO), modo do motor 1 = meia onda, 2 = ponte; 0 V com
// alfa >= 179 graus (sem disparo). As mesmas contas no "?" e na pagina.
float scrVoMedTeorica(uint8_t m, float alfaGraus);
float scrVoRmsTeorica(uint8_t m, float alfaGraus);
const char *scrPegarMotivo();         // motivo do ultimo desligamento automatico (uma vez), ou nullptr
