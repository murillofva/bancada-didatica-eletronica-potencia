/*
  web.h - Painel web pelo Wi-Fi.

  A placa cria a rede "Bancada-TCC" (senha em config.h). Em qualquer aparelho com
  Wi-Fi (computador, celular ou tablet), conecte nessa rede e abra http://192.168.4.1. O painel mostra o modo, o estado da
  saida, as medidas ao vivo, graficos (historico de 1 s e leitura rapida dos
  INA226) e controla o REGISTRO CSV (iniciar, parar, baixar, apagar, salvar).

  Controle pela pagina: um aparelho por vez "pega o controle" e manda os mesmos
  comandos da serial (modo, l, d, a, f, rampa, p, r, zero, tela).
  Homem-morto: sem sinal de vida por WEB_HOMEM_MORTO_MS, a saida desliga e o
  controle e solto. Com PAINEL_MANUAL 0 (sem pots, o padrao) ele
  fica livre para outro aparelho ou para a serial; com PAINEL_MANUAL 1 ele volta
  aos potenciometros. O "Soltar" faz o mesmo. DESLIGA TUDO (/cmd?c=0) funciona em
  qualquer aparelho.

  O servidor roda numa tarefa propria no nucleo 0 (o mesmo do Wi-Fi). O loop
  (nucleo 1, onde ficam o controle e as interrupcoes dos SCRs) so copia um
  "retrato" das medidas a cada 200 ms (webAtualizar).
*/
#pragma once
#include <Arduino.h>
#include "config.h"

void webIniciar();          // no setup, por ultimo: cria a rede e o servidor
void webAtualizar();        // chamar sempre no loop
bool webAtivo();
bool webNoControle();       // algum aparelho esta com o controle da pagina (com sinal de vida)
void webMostrar();          // comando "wifi" na serial
