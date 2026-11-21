/*
  comandos.h - Monitor Serial (115200 baud, final de linha "Nova linha"):
  leitura dos comandos sem travar o loop, "?" (estado), "ajuda" e o log CSV.
*/
#pragma once
#include <Arduino.h>
#include "config.h"

// De onde veio um comando. Muda so o "l" (com PAINEL_MANUAL 0): a serial e a caixa
// de comandos da pagina pedem o segundo "l"; o botao "Ligar a saida" da pagina
// ja pediu confirmacao na janela dele e liga direto.
enum OrigemCmd : uint8_t { ORIGEM_SERIAL = 0, ORIGEM_WEB_BOTAO, ORIGEM_WEB_CAIXA };

void comandosIniciar();
void comandosAtualizar();          // chamar sempre no loop: le a serial e imprime o log
void ajuda();
void comandosCancelarLiga();       // esquece um "l" da serial esperando confirmacao (DESLIGA TUDO, homem-morto)
void mostrarEstado();              // comando "?"
bool lerNumero(String arg, float &valor);
// Vo teorica (ideal) dos conversores com a Vin medida e o D atual: a mesma conta do "?"
// e da pagina. Boost: vo = Vin/(1-D), NAN com D >= 0,99. Buck:
// vo = D x Vin e voAtraso = dq1 x Vin, com dq1 = o D no Q1 com o atraso tipico do IRS2104.
// O que nao se aplica (ou sem a Vin) sai NAN.
void teoVoConversor(bool boost, float vin, float d, float &vo, float &voAtraso, float &dq1);
const char *csvCabecalho();             // cabecalho do CSV (log da serial e registro)
String csvLinha();                      // uma linha do CSV com as medias da ultima janela de 1 s

// Mensagem que aparece "sozinha" (nao e resposta a um comando): com o log
// ligado sai com "# " na frente, para nao misturar com as linhas do CSV.
// Use uma chamada por linha: aviso().println("...") ou aviso().printf("...\n").
Print &aviso();

// Saida de texto dos comandos: vai para a serial e, durante um comando vindo do
// painel web, tambem fica guardada para a resposta da pagina. Use con().printf(...)
// no lugar de Serial.printf(...) nas respostas de comandos. So no loop (nucleo 1).
Print &con();

// Marca de tempo desde o reset, no inicio da linha: "[t=1234.5 s] "
// (segundos com uma casa, cortada: 95,99 s -> 95.9). comTempo escreve a marca em p e
// devolve p: comTempo(aviso()).printf("...\n") -> "# [t=95.3 s] ..." com o log ligado.
// Com ms, marca esse instante (um valor de millis()) e nao agora. marcaTempo poe o
// mesmo texto num buffer (24 bytes bastam), para montar a linha num printf so.
Print &comTempo(Print &p);
Print &comTempo(Print &p, uint32_t ms);
char *marcaTempo(char *buf, size_t n, uint32_t ms);

// ---- Saida unica da serial ----
// Todo texto da bancada vai para a serial por aqui (o con() tambem passa por aqui).
// Cada byte tambem e copiado para o ESPELHO: um buffer circular que a pagina
// le pela rota /serial (web.cpp). Pode ser usada em qualquer
// tarefa (a copia tem uma trava curta, um mutex do FreeRTOS que nao bloqueia
// interrupcoes); NUNCA de dentro de uma interrupcao. Fora do loop (ex.: tarefa do
// servidor web, nucleo 0), use esta e nao o con().
Print &saidaSerial();
void espelhoIniciar();             // logo depois do Serial.begin(), no comeco do setup

// Copia o texto do espelho a partir do byte "desde" (contado desde o reset) para dst,
// ate max bytes. Devolve quantos copiou. ini = de onde copiou (pode ser maior que
// "desde", se esse trecho ja foi sobrescrito: perdeu = true); total = bytes escritos
// desde o reset; reinicio = "desde" era maior que total (a placa reiniciou).
size_t espelhoLer(uint32_t desde, char *dst, size_t max, uint32_t &ini, uint32_t &total, bool &perdeu, bool &reinicio);

// Executa um comando de texto (mesma sintaxe da serial) vindo do painel web e
// devolve o que ele escreveu (ate ~4 kB; mais que isso sai cortado, com aviso).
// caixa = veio da caixa "Comando (igual a serial)", e nao de um botao da pagina.
// Chamar SO no loop (nucleo 1), nunca no servidor.
String comandoWeb(const char *txt, bool caixa);
