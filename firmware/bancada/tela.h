/*
  tela.h - OLED SSD1306 128x64 (I2C 0x3C), biblioteca U8g2.

  Paginas:
    0 - principal (letras grandes): Vo, Io, eta  /  Vo_med, Io_med, rede
    1 - complemento: Vin, Iin, Pin, Pout  /  Vo_rms, Io_rms, eta, Vo/R
    2 - estado: quem controla, I2C, rede, calibracao
  Escolha da pagina:
    - troca sozinha a cada TELA_TROCA_MS (padrao com PAINEL_MANUAL 0);
    - comando "tela 0|1|2" (serial ou pagina web) fixa uma pagina; "tela auto" volta a trocar;
    - com PAINEL_MANUAL 1: Pot 3 e botao MODO apertado por 0,8 s (fixam a pagina).
  O cabecalho (faixa clara em cima) mostra sempre: modo, LIG/DESL e o
  valor ajustado (D ou alfa).

  A tela e desenhada na memoria e enviada em 8 pedacos (uma faixa de 8 linhas
  de pixels por volta do loop), para nao segurar o loop por ~25 ms.
*/
#pragma once
#include <Arduino.h>
#include "config.h"

void telaIniciar();                // procura o OLED e inicia (no setup, depois do Wire.begin)
void telaDetectar(bool imprimir);   // procura de novo (comando "i2c")
bool telaPresente();
void telaAtualizar();               // chamar sempre no loop
void telaProximaPagina();           // proxima pagina e fixa nela (botao MODO longo)
void telaDefinirPagina(uint8_t p);  // fixa a pagina (desliga a troca automatica)
void telaAuto();                    // volta a trocar de pagina sozinha
bool telaEhAuto();
uint8_t telaPagina();
// Mensagem por cima das medidas (ate 3 linhas de 18 letras) por "ms" milissegundos.
void telaMensagem(const char *l1, const char *l2 = nullptr, const char *l3 = nullptr, uint32_t ms = TELA_MSG_MS);
