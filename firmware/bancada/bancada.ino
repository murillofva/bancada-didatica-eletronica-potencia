/*
  Firmware unificado da bancada didatica de eletronica de potencia
  (TCC IFSP Catanduva)

  Um so programa para os cinco experimentos da bancada:
    BOOST     - conversor Boost (PWM no GPIO15 -> TC4427)
    BUCK      - conversor Buck sincrono (GPIO16 -> IN e GPIO17 -> SD do IRS2104)
    RET_NC    - retificadores nao controlados (diodos): so medicao
    RET_MEIA  - retificador de meia onda controlado (SCR A)
    RET_PONTE - ponte semicontrolada (SCR A e SCR B)

  Medicoes pelo I2C: INA226 na entrada (0x40) e na saida (0x41) dos
  conversores; ADS1115 (0x48) com divisor de tensao e ACS712 nos
  retificadores. Tela OLED 128x64 (0x3C) e 2 LEDs. Comando pela pagina web
  e pela serial. Os 3 botoes e os 3 potenciometros sao opcionais: chave
  PAINEL_MANUAL no config.h (0 = sem eles, o padrao).

  Ligacoes, seguranca, comandos, calibracao e roteiro de validacao: LEIA-ME.md.

  Placa: ESP32-S3 DevKitC-1 N16R8. Na IDE: "ESP32S3 Dev Module", Flash Size 16MB,
  PSRAM "OPI PSRAM", Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)",
  USB CDC On Boot "Disabled" (cabo na porta USB marcada COM). Painel web
  pelo Wi-Fi e registro CSV na PSRAM.
  Core Arduino "esp32" 3.3.x. Biblioteca externa: U8g2 (so para a tela).
  Monitor Serial: 115200 baud, final de linha "Nova linha". Digite "ajuda".

  Arquivos:
    config.h            pinos, enderecos, limites e valores padrao (TUDO aqui)
    pwm_conversores.*   PWM do Boost e do Buck (LEDC), rampa de D
    disparo_scr.*       disparo dos SCRs sincronizado com a rede
    medicao.*           drivers do INA226 e do ADS1115, medias, calibracao (NVS)
    tela.*              OLED (U8g2)
    painel.*            LEDs e (com PAINEL_MANUAL 1) botoes e potenciometros
    controle.*          modos, liga/desliga com trava, protecoes
    comandos.*          Monitor Serial, "?" e log CSV
    registro.*          historico, leituras rapidas e registro CSV na PSRAM
    web.*               rede Wi-Fi "Bancada-TCC" e painel em http://192.168.4.1

  Ao ligar ou resetar: TODAS as saidas desligadas (PWMs em 0, SD do Buck em
  LOW, gates dos SCRs em LOW), modo RET_NC (so medicao). Controle pela serial
  e pela pagina web (com PAINEL_MANUAL 1: pelos potenciometros).
*/
#include <Arduino.h>
#include "config.h"
#include "pwm_conversores.h"
#include "disparo_scr.h"
#include "medicao.h"
#include "tela.h"
#include "painel.h"
#include "controle.h"
#include "comandos.h"
#include "registro.h"
#include "web.h"

void setup() {
  // 1) Antes de tudo: todas as saidas de potencia desligadas.
  pinMode(PINO_BUCK_SD, OUTPUT);
  digitalWrite(PINO_BUCK_SD, LOW);
  pinMode(PINO_BOOST_PWM, OUTPUT);
  digitalWrite(PINO_BOOST_PWM, LOW);
  pinMode(PINO_BUCK_PWM, OUTPUT);
  digitalWrite(PINO_BUCK_PWM, LOW);
  pinMode(PINO_GATE_A, OUTPUT);
  digitalWrite(PINO_GATE_A, LOW);
  pinMode(PINO_GATE_B, OUTPUT);
  digitalWrite(PINO_GATE_B, LOW);

  Serial.setTxBufferSize(SERIAL_BUFFER_TX);  // precisa vir antes do begin()
  Serial.begin(SERIAL_BAUD);
  espelhoIniciar();    // espelho da serial para a pagina, desde a primeira linha
  delay(300);
  // Daqui em diante, todo texto sai por con() / saidaSerial(): serial e espelho.
  con().println();
  con().println("=== Bancada didatica de eletronica de potencia - firmware unificado ===");
  con().println("Placa: " PLACA_NOME);
  con().println("Versao do firmware: " FW_VERSAO " (" FW_DATA ")");

  calCarregar();       // calibracoes gravadas (ou os padroes do config.h)
  pwmIniciar();        // LEDC do Boost e do Buck, D = 0, SD em LOW
  scrIniciar();        // timers e interrupcoes do disparo dos SCRs (gates em LOW)
  painelIniciar();     // LEDs (e, com PAINEL_MANUAL 1, botoes e potenciometros)
  medicaoIniciar();    // I2C: INA226 x2 e ADS1115
  telaIniciar();       // OLED
  controleIniciar();   // modo RET_NC, tudo desligado
  comandosIniciar();
  registroIniciar();   // historico, rapido e registro CSV na PSRAM
  webIniciar();        // rede Wi-Fi "Bancada-TCC" e painel em http://192.168.4.1 (nucleo 0)
  if (webAtivo()) telaMensagem("Wi-Fi " , WIFI_SSID, "http://192.168.4.1", 5000);

#if PAINEL_MANUAL
  con().println("Painel manual LIGADO (PAINEL_MANUAL 1): botoes MODO, LIGA e ZERO e potenciometros 1 a 3.");
  con().println("Tudo DESLIGADO. Modo RET_NC (so medicao); botao MODO ou \"modo ...\" troca o modo.");
#else
  con().println("Sem botoes e sem potenciometros (PAINEL_MANUAL 0): comando pela pagina web e pela serial.");
  con().println("Tudo DESLIGADO. Modo RET_NC (so medicao); \"modo ...\" (serial ou pagina) troca o modo.");
#endif
  calMostrar();
  ajuda();
}

void loop() {
  comandosAtualizar();   // serial e log CSV
  painelAtualizar();     // botoes e potenciometros (com PAINEL_MANUAL 0 nao faz nada)
  controleAtualizar();   // rampa, alvos, vigia da rede, protecoes, LEDs
  medicaoAtualizar();    // INA226 (50 ms), ADS1115 (continuo), janela de 1 s
  telaAtualizar();       // OLED (uma faixa por volta)
  registroAtualizar();   // historico, rapido e registro CSV
  webAtualizar();        // retrato das medidas para o painel web
}
