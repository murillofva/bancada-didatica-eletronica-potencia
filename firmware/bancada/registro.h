/*
  registro.h - Historico, leituras rapidas e registro CSV na PSRAM.

  Tres "memorias", todas na PSRAM de 8 MB:
    - historico: um ponto por janela de 1 s (as mesmas medias da tela e do "?"), 1 hora;
    - rapido:    cada leitura dos INA226 (~50 ms), 2 minutos (transitorios dos conversores);
    - registro:  texto CSV, uma linha a cada periodo escolhido (comando "grava" ou painel web).

  Nada disso escreve na flash durante o experimento (escrever na flash pausa o
  processador por alguns ms e atrapalharia o disparo dos SCRs). A copia para a
  flash ("salva" / botao do painel) so e feita com a saida DESLIGADA.

  Um so "escritor" (o loop, nucleo 1) e leitores no painel web (nucleo 0): os
  leitores copiam sem trava e conferem se o dado nao foi sobrescrito no meio.
  Pedidos do painel (iniciar, parar, apagar, salvar) viram "pedidos" atendidos
  pelo loop, para que so ele mexa nas memorias.
*/
#pragma once
#include <Arduino.h>
#include "config.h"

struct PontoHist {
  uint32_t id;          // numero da janela de 1 s
  uint32_t tms;         // millis() no fim da janela
  uint8_t  modo;        // Modo (controle.h)
  uint8_t  ligado;      // 1 = saida ligada
  float d, alfa;        // D atual (conversores) e alfa atual (SCR); NAN quando nao se aplica
  float vin, iin, vo, io, pin, pout, eta;  // INA226 (conversores)
  float voMed, voRms, ioMed, ioRms, etaRet; // ADS1115 (retificadores)
  float fRede;          // Hz (0 sem sincronismo)
};

struct PontoRapido {
  uint32_t n;           // numero da leitura dos INA226
  uint32_t tms;
  float vin, iin, vo, io;
};

void registroIniciar();     // no setup, depois de medicaoIniciar()
void registroAtualizar();   // chamar sempre no loop

// ---- Registro CSV ----
bool     registroGravando();
uint32_t registroPeriodoMs();
uint32_t registroLinhas();
size_t   registroBytes();      // tamanho do texto (cabecalho incluido)
size_t   registroCapacidade();
bool     registroCheio();
// Pedidos (podem vir do painel web, no outro nucleo): o loop atende na proxima volta.
void registroPedirInicio(uint32_t periodoMs);
void registroPedirParada();
void registroPedirApagar();
bool registroPedirSalvar();    // false se a saida estiver ligada (salvar so com tudo desligado)
const char *registroMensagem(); // resultado do ultimo pedido (ex.: "salvo: 12345 bytes")
bool     registroSalvando();
bool     registroTemSalvo();
size_t   registroTamanhoSalvo();
// Leitura do texto CSV por pedacos (painel web). Devolve quantos bytes copiou.
size_t   registroLerTexto(size_t pos, char *dst, size_t max);

// ---- Historico e rapido (painel web) ----
uint32_t histUltimoId();                       // 0 = vazio
uint32_t histCopiar(uint32_t desdeId, PontoHist *dst, uint32_t max);   // pontos com id > desdeId
uint32_t rapidoUltimoN();
uint32_t rapidoCopiar(uint32_t desdeN, PontoRapido *dst, uint32_t max);
bool     registroPsramOk();                    // false = PSRAM nao encontrada (memorias menores)
