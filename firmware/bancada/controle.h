/*
  controle.h - Modos da bancada, liga/desliga com trava de seguranca,
  potenciometros x serial, protecoes e LEDs.

  Modos (botao MODO ou "modo ..."):
    BOOST     - conversor Boost (GPIO15)
    BUCK      - conversor Buck sincrono (GPIO16/17)
    RET_NC    - retificadores NAO controlados (diodos): so medicao
    RET_MEIA  - meia onda controlada (SCR A, GATE_A)
    RET_PONTE - ponte semicontrolada (SCR A e B, GATE_A/GATE_B)
  Trocar de modo SEMPRE desliga todas as saidas antes.

  Com PAINEL_MANUAL 0 (padrao) nao ha botoes nem pots: o controle e sempre
  "pela serial" (serial e pagina web, como o "pot 0"), e "pot 1" e recusado
  com aviso. Com PAINEL_MANUAL 1, os pots e a serial ("pot 1" / "pot 0").
*/
#pragma once
#include <Arduino.h>
#include "config.h"

enum Modo : uint8_t { MODO_BOOST = 0, MODO_BUCK, MODO_RET_NC, MODO_RET_MEIA, MODO_RET_PONTE, NUM_MODOS };

void controleIniciar();
void controleAtualizar();                 // chamar sempre no loop

Modo modoAtual();
const char *nomeModo(Modo m);             // "BOOST", "BUCK", "RET_NC", ...
const char *nomeModoTela(Modo m);         // "BOOST", "BUCK", "RET NC", ...
bool modoConversor(Modo m);               // BOOST ou BUCK
bool modoScr(Modo m);                     // RET_MEIA ou RET_PONTE

void controleMudarModo(Modo m);

bool saidaLigada();
bool controleLigar();                     // pots: com a trava; serial/pagina: parte de D = 0 / alfa = 180
void controleDesligar(const char *motivo);// motivo = nullptr: desligado pelo usuario

#if PAINEL_MANUAL
void controleProximoModo();               // botao MODO
void controleAlternar();                  // botao LIGA-DESLIGA
#endif

bool controlePeloPot();                   // sempre false com PAINEL_MANUAL 0
bool controleUsarPot(bool sim);           // "pot 1" / "pot 0" ("pot 1" recusado com PAINEL_MANUAL 0)
bool controleDefinirD(float d);           // "d" (so com controle pela serial)
bool controleDefinirAlfa(float graus);    // "a" (so com controle pela serial)
bool controleDefinirFrequencia(uint32_t hz);
void controleZero();                      // botao ZERO / "zero"

float controleAlvo();                     // D alvo ou alfa alvo do modo atual
bool controlePotForaDoZero();             // controle pelo pot, saida desligada e pot fora do zero (false sem pots)
const char *controleUltimaProtecao();     // "" se nenhuma (o "?" mostra sempre o ultimo)
uint32_t controleUltimaProtecaoMs();      // millis() desse desligamento
// O mesmo motivo, mas so ate a saida ser ligada de novo ou um aparelho pegar o
// controle da pagina; depois, "". E o que a pagina (cartao) e a tela OLED mostram.
const char *controleProtecaoAtual();
void controleApagarProtecaoAtual();       // chamar no laco (o web.cpp chama ao pegar o controle)

// ---- Limites de protecao de corrente (comando "lim": serial e caixa da pagina) ----
// Comecam no limite efetivo: o menor entre o config.h e 95 % do fundo de
// escala do INA226 (81,92 mV / Rsh calibrado). O "lim" so BAIXA os limites de
// corrente (de LIM_TESTE_MIN_A ate o efetivo), so com a saida desligada e so na RAM:
// reset ou religar a placa volta ao config.h. BOOST_VO_MAX nao muda.
// Vshunt no fundo de escala, com a saida ligada, desliga a saida.
enum LimiteCorrente : uint8_t { LIM_IIN, LIM_IO };     // Iin = Boost e Buck; Io = Buck
void controleMostrarLimites();                          // "lim"
bool controleDefinirLimite(LimiteCorrente qual, float a); // "lim iin <A>" / "lim io <A>"
bool controleLimitesPadrao();                           // "lim padrao"
// Escreve "ATENCAO: limite de teste ativo: ..." se houver limite de teste
// (soDoModo = so os que valem no modo atual). Devolve true se escreveu.
bool controleAvisoLimiteTeste(Print &p, bool soDoModo);
