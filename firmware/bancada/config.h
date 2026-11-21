/*
  config.h - TODOS os pinos, enderecos I2C, limites e valores padrao da bancada.

  Para mudar um pino, um limite de protecao ou uma calibracao padrao, mude
  AQUI. As calibracoes feitas pela serial (comando "cal") ficam gravadas na
  memoria do ESP32 (NVS) e tem prioridade sobre os valores padrao abaixo;
  "cal padrao" volta para estes valores.
*/
#pragma once
#include <Arduino.h>

// =====================================================================
//  VERSAO DO FIRMWARE
// =====================================================================
// Mude a cada alteracao: X.0.0 = troca de placa ou de pinos; 0.X.0 = funcao
// nova; 0.0.X = correcao pequena.
#define FW_VERSAO "2.3.5"
#define FW_DATA   "2026-11-21"

// =====================================================================
//  PAINEL MANUAL (potenciometros e botoes)
// =====================================================================
// 0 = SEM potenciometros e SEM botoes (padrao): o comando e so pela pagina web
//     e pela serial (reserva). O programa NAO le os pinos dos pots nem dos
//     botoes; a pagina da tela OLED troca sozinha (ou pelo comando "tela").
// 1 = com Pot 1 (D), Pot 2 (alfa), Pot 3 (pagina da tela) e
//     botoes MODO, LIGA-DESLIGA e ZERO, com a trava do LIGA pelos pots.
// A placa C continua com os conectores J31-J33 (pots) e J34-J36 (botoes): com 0,
// eles podem ficar sem nada ligado.
#ifndef PAINEL_MANUAL
#define PAINEL_MANUAL 0
#endif
static_assert(PAINEL_MANUAL == 0 || PAINEL_MANUAL == 1, "PAINEL_MANUAL precisa ser 0 ou 1");

// =====================================================================
//  PINOS - ESP32-S3 DevKitC-1 N16R8 (44 pinos, 16 MB de flash, 8 MB de PSRAM)
// =====================================================================
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Este firmware e para a ESP32-S3 DevKitC-1 N16R8: escolha \"ESP32S3 Dev Module\" na IDE"
#endif
// Livres de proposito: GPIO 0, 3, 45 e 46 (pinos de boot), 19 e 20 (USB nativa),
// 26 a 37 (flash e PSRAM octal), 43 e 44 (serial do conversor USB "COM"),
// 38 e 48 (LED RGB da placa, conforme a versao). Os pots ficam no ADC1 (GPIO 1 a 10),
// que funciona junto com o Wi-Fi.
#define PLACA_NOME "ESP32-S3 DevKitC-1 N16R8"
const int PINO_BOOST_PWM  = 15;  // -> TC4427 IN A (10 kOhm do pino ao GND)
const int PINO_BUCK_PWM   = 16;  // -> IRS2104 IN (10 kOhm do pino ao GND)
const int PINO_BUCK_SD    = 17;  // -> IRS2104 SD (barrado). LOW = driver desligado (10 kOhm ao GND)
const int PINO_GATE_A     = 39;  // -> 220 Ohm -> LED do MOC3023 do SCR A (10 kOhm ao GND)
const int PINO_GATE_B     = 40;  // -> 220 Ohm -> LED do MOC3023 do SCR B (10 kOhm ao GND)
const int PINO_ZC         = 41;  // <- coletor do H11AA1 (pull-up EXTERNO de 10 kOhm)
const int PINO_POL        = 42;  // <- coletor do PC817  (pull-up EXTERNO de 10 kOhm)
const int PINO_SDA        = 8;   // I2C: INA226 x2, ADS1115, OLED
const int PINO_SCL        = 9;
#if PAINEL_MANUAL  // com 0 estes pinos nem existem no programa: nada le IO1, IO2, IO4, IO5, IO6 e IO7
const int PINO_POT_D      = 1;   // Pot 1 (ADC1): razao ciclica D (Boost/Buck)      - J31 da placa C
const int PINO_POT_ALFA   = 2;   // Pot 2 (ADC1): angulo alfa (retificadores)       - J32
const int PINO_POT_3      = 4;   // Pot 3 (ADC1): escolhe a pagina da tela          - J33
const int PINO_BOTAO_MODO = 5;   // botoes: entre o pino e o GND (pull-up interno)  - J34
const int PINO_BOTAO_LIGA = 6;   //                                                  - J35
const int PINO_BOTAO_ZERO = 7;   //                                                  - J36
#endif
const int PINO_LED_SAIDA  = 14;  // LED "saida ligada" (330 Ohm a 1 kOhm)
const int PINO_LED_SINC   = 21;  // LED "sincronizado com a rede"
// Nomes dos pinos nas mensagens da serial (texto fixo, sem custo em tempo de execucao)
#define TXT_PWM_BOOST "GPIO15"
#define TXT_PWM_BUCK  "GPIO16"
#define TXT_SD_BUCK   "GPIO17"
#define TXT_ZC        "GPIO41"
#define TXT_POL       "GPIO42"

// O disparo dos SCRs escreve e le direto no "banco 1" de registradores do GPIO
// (pinos 32 em diante): os gates, o ZC e o POL precisam ficar nesse banco.
static_assert(PINO_GATE_A >= 32 && PINO_GATE_B >= 32 && PINO_ZC >= 32 && PINO_POL >= 32,
              "GATE_A, GATE_B, ZC e POL precisam ser GPIO 32 ou maior (banco 1)");

// =====================================================================
//  PAINEL WEB E REGISTRO
// =====================================================================
const char     WIFI_SSID[]       = "Bancada-TCC";   // rede criada pela propria placa
const char     WIFI_SENHA[]      = "bancada2026";   // minimo de 8 caracteres
const uint8_t  WIFI_CANAL        = 6;
const uint8_t  WIFI_MAX_CLIENTES = 6;               // aparelhos ao mesmo tempo
const uint16_t WEB_PORTA         = 80;              // http://192.168.4.1
const uint32_t WEB_HOMEM_MORTO_MS = 2000;           // painel web no controle sem sinal por 2 s -> desliga a saida e solta o controle
const uint32_t HIST_PONTOS       = 3600;            // historico de 1 s em 1 s: 1 hora (na PSRAM)
const uint32_t RAPIDO_PONTOS     = 2400;            // INA226 a cada ~50 ms: 2 minutos (na PSRAM)
const size_t   REG_BYTES_MAX     = 4UL * 1024 * 1024; // registro CSV na PSRAM: 4 MB (~35 mil linhas)
const uint32_t REG_PERIODO_PADRAO_MS = 1000;
const char     REG_ARQ_SALVO[]   = "/registro_salvo.csv";  // copia na flash (so com a saida desligada)
// Espelho do Monitor Serial na pagina: todo texto que vai para a
// serial e copiado num buffer circular na PSRAM, desde o inicio do setup. A pagina
// le pela rota /serial?desde=N, ate ESPELHO_RESP_MAX bytes por vez.
const size_t   ESPELHO_BYTES     = 64UL * 1024;     // buffer circular (potencia de 2): ~800 linhas
const size_t   ESPELHO_BYTES_SEM_PSRAM = 8UL * 1024; // se a PSRAM faltar: na RAM interna
const size_t   ESPELHO_RESP_MAX  = 8UL * 1024;      // bytes por resposta da rota /serial
static_assert((ESPELHO_BYTES & (ESPELHO_BYTES - 1)) == 0 && (ESPELHO_BYTES_SEM_PSRAM & (ESPELHO_BYTES_SEM_PSRAM - 1)) == 0,
              "os tamanhos do espelho precisam ser potencias de 2");

// =====================================================================
//  SERIAL
// =====================================================================
const uint32_t SERIAL_BAUD      = 115200;
const size_t   SERIAL_BUFFER_TX = 4096;  // o "?" nao trava o loop enquanto a serial envia
const unsigned LINHA_MAX        = 60;    // tamanho maximo de um comando
const uint32_t LOG_MIN_MS       = 100;
const uint32_t LOG_MAX_MS       = 3600000UL;

// =====================================================================
//  I2C
// =====================================================================
const uint32_t I2C_FREQ_HZ     = 400000;
const uint16_t I2C_TIMEOUT_MS  = 20;     // um dispositivo travado nao segura o programa
const uint8_t  END_INA_ENTRADA = 0x40;   // INA226 #1: entrada do conversor (Vin, Iin)
const uint8_t  END_INA_SAIDA   = 0x41;   // INA226 #2: saida do conversor (Vo, Io) - A0 ligado ao VS
const uint8_t  END_ADS         = 0x48;   // ADS1115: retificadores (ADDR no GND)
const uint8_t  END_OLED        = 0x3C;   // SSD1306 128x64
const uint32_t REDETECTAR_MS   = 3000;   // procura de novo INA226/ADS1115 ausentes a cada 3 s

// ---- INA226 (driver por registrador, sem biblioteca) ----
const uint8_t  INA_REG_CONFIG  = 0x00;
const uint8_t  INA_REG_SHUNT   = 0x01;   // tensao no shunt, com sinal, 2,5 uV por bit
const uint8_t  INA_REG_BUS     = 0x02;   // tensao do barramento (pino VBUS), 1,25 mV por bit
const uint8_t  INA_REG_ID_FAB  = 0xFE;   // "TI" = 0x5449
const uint16_t INA_ID_TI       = 0x5449;
// Configuracao: media de 16 conversoes (AVG = 010), tempo de conversao de
// 1,1 ms no barramento (VBUSCT = 100) e no shunt (VSHCT = 100), modo continuo
// shunt + barramento (MODE = 111). Bit 14 e reservado e fica em 1.
// Um resultado novo a cada 16 x (1,1 + 1,1) ms = 35,2 ms.
const uint16_t INA_CONFIG      = 0x4000 | (2u << 9) | (4u << 6) | (4u << 3) | 7u;  // = 0x4527
const float    INA_LSB_SHUNT_V = 2.5e-6f;
const float    INA_LSB_BUS_V   = 1.25e-3f;
// Fundo de escala do shunt: +-32768 x 2,5 uV = +-81,92 mV. Acima disso o registrador
// trava em 0x7FFF (ou 0x8000) e a corrente lida trava em 81,92 mV / Rsh, por maior
// que seja a real (com o Rsh1 calibrado de 0,109 Ohm, em ~0,75 A). Por isso, nos
// conversores, com a saida ligada, |registrador| >= INA_SHUNT_SAT_CRU desliga
// a saida como sobrecorrente ("no fundo de escala").
const float    INA_FUNDO_ESCALA_V = 32768.0f * INA_LSB_SHUNT_V;  // 81,92 mV
const int16_t  INA_SHUNT_SAT_CRU  = 32600;   // = 81,5 mV (99,5 % do fundo de escala)
const uint32_t INA_PERIODO_MS  = 50;     // le os dois INA226 a cada 50 ms
const uint8_t  INA_FALHAS_MAX  = 3;      // 3 leituras seguidas com erro = "nao responde"

// ---- ADS1115 (driver por registrador, sem biblioteca) ----
const uint8_t  ADS_REG_CONV    = 0x00;
const uint8_t  ADS_REG_CONFIG  = 0x01;
// Conversao unica (MODE = 1), PGA +-4,096 V (001), 860 amostras/s (111),
// comparador desligado (COMP_QUE = 11). OS = 1 inicia a conversao.
const uint16_t ADS_CFG_BASE    = 0x0200 | 0x0100 | 0x00E0 | 0x0003;
const uint16_t ADS_OS          = 0x8000;
const uint16_t ADS_MUX_AIN0    = 0x4000;  // A0 (entrada simples): tensao do retificador (divisor)
const uint16_t ADS_MUX_AIN1    = 0x5000;  // A1 (entrada simples): saida do ACS712
const float    ADS_LSB_V       = 4.096f / 32768.0f;  // 125 uV por bit
const uint32_t ADS_ESPERA_US   = 1200;    // conversao de 1/860 s = 1,16 ms (+ folga)
const uint32_t ADS_TIMEOUT_US  = 10000;
const uint32_t ADS_MEIO_US     = 580;     // meio da conversao, contado do fim do comando
const uint32_t ADS_DITHER_US   = 800;     // espera aleatoria de 0 a 0,8 ms antes de cada conversao
const uint8_t  ADS_FALHAS_MAX  = 5;
const float    ADS_LIMITE_A0_V = 3.25f;   // A0 acima disso: perto do limite do ADS1115 (VDD = 3,3 V)

// ---- Media e RMS dos retificadores ----
const uint32_t JANELA_MS          = 1000; // todas as medias (INA226 e ADS1115) sao de 1 s
const uint16_t ADS_MAX_AMOSTRAS   = 600;  // amostras guardadas por canal por janela (~250 usadas)
const uint16_t ADS_MIN_AMOSTRAS   = 50;   // menos que isso: usa a media simples

// =====================================================================
//  CALIBRACOES PADRAO (as gravadas pela serial tem prioridade)
// =====================================================================
const float RSH1_PADRAO     = 0.100f;                  // Ohm, shunt R100 do INA226 #1
const float RSH2_PADRAO     = 0.100f;                  // Ohm, shunt R100 do INA226 #2
const float DIV_PADRAO      = (100.0f + 8.2f) / 8.2f;  // divisor 100 k + 8,2 k: Vo = V_A0 x 13,195
const float ACS_SENS_PADRAO = 0.185f;                  // V/A, ACS712-5A alimentado com 5 V
const float ACS_ZERO_PADRAO = 2.500f;                  // V na saida do ACS712 com 0 A
const float RCARGA_PADRAO   = 47.0f;                   // Ohm, carga do retificador (Io = Vo/R)
const char  NVS_NOME[]      = "bancada";               // "pasta" das calibracoes na NVS

// =====================================================================
//  CONVERSORES CC-CC (PWM por hardware, periferico LEDC)
// =====================================================================
const float    BOOST_DMAX       = 0.50f;   // limite de seguranca do Boost na protoboard
const uint32_t BOOST_F_PADRAO   = 100000;
const uint32_t BOOST_F_MIN      = 20000;
const uint32_t BOOST_F_MAX      = 150000;
const float    BUCK_DMAX        = 0.90f;   // o Q2 precisa ligar em todo ciclo (bootstrap)
const uint32_t BUCK_F_PADRAO    = 50000;
const uint32_t BUCK_F_MIN       = 20000;
const uint32_t BUCK_F_MAX       = 100000;  // acima disso o tempo morto do IRS2104 pesa demais
const float    PASSO_RAMPA_D    = 0.005f;  // rampa de D: 0,005 a cada 10 ms (0,5 por segundo)
const uint32_t PERIODO_RAMPA_MS = 10;
const float    BUCK_ENCURTAMENTO_Q1_US = 0.53f;  // atraso tipico do IRS2104 (so para o "?")

// ---- Exigir os dois INA226 nos conversores (Boost e Buck) ----
// 1 = o LIGA e recusado sem os dois INA226 respondendo, e a saida desliga se um deles
//     parar de responder ("INA226 #1 mudo" / "INA226 #2 mudo"). E o normal da bancada
//     montada: sem os dois INA226 respondendo, o Boost e o Buck nao ligam. A protecao
//     "INA226 mudo" so existe com 1.
// 0 = liga mesmo sem os INA226.
//     Os limites abaixo so valem para o INA226 que estiver respondendo; sem nenhum,
//     NAO HA PROTECAO de corrente nem de tensao: limite a corrente na fonte.
#ifndef EXIGIR_INA226
#define EXIGIR_INA226 1
#endif
static_assert(EXIGIR_INA226 == 0 || EXIGIR_INA226 == 1, "EXIGIR_INA226 precisa ser 0 ou 1");

// ---- Protecoes (testadas a cada leitura nova dos INA226, ~50 ms) ----
// Corrente: 0,70 A. Os shunts medidos deram Rsh1 = 0,109 Ohm e Rsh2 = 0,107 Ohm
// (trilhas e bornes do modulo entram junto), e o fundo de escala fica em ~0,75 A:
// um limite acima disso nunca atuaria. Alem disso, cada limite fica no MENOR entre o valor abaixo
// e LIM_FRACAO_FUNDO do fundo de escala do INA226 que o mede (81,92 mV / Rsh
// calibrado, arredondado para baixo em 0,01 A): um Rsh maior nunca deixa o limite
// inalcancavel. E o "limite efetivo" (comando lim).
const float BOOST_VO_MAX  = 30.0f;  // V (INA226 #2). O INA226 aguenta 36 V.
const float BOOST_IIN_MAX = 0.70f;  // A (INA226 #1)
const float BUCK_IIN_MAX  = 0.70f;  // A (INA226 #1)
const float BUCK_IO_MAX   = 0.70f;  // A (INA226 #2)
const float LIM_FRACAO_FUNDO = 0.95f;  // limite efetivo <= 95 % do fundo de escala
const float ETA_PIN_MIN_W = 0.05f;  // abaixo disso o rendimento nao e calculado

// ---- Limite de TESTE pelo comando "lim" da serial ----
// Para testar as protecoes de corrente com corrente baixa:
// "lim io <A>" baixa o BUCK_IO_MAX; "lim iin <A>" baixa o BOOST_IIN_MAX e o
// BUCK_IIN_MAX. Aceita de LIM_TESTE_MIN_A ate o limite efetivo, em passos de 0,01 A,
// so com a saida desligada. Fica SO NA RAM: "lim padrao", reset ou religar a placa
// voltam aos valores acima. O BOOST_VO_MAX nao muda por comando. Vale pela serial e
// pela caixa de comandos da pagina (para mudar, com o controle da pagina).
const float LIM_TESTE_MIN_A = 0.05f;

// =====================================================================
//  LIGAR PELA SERIAL SEM OS POTENCIOMETROS (PAINEL_MANUAL 0)
// =====================================================================
// A pagina pede confirmacao antes de ligar. Na serial, a confirmacao e mandar
// "l" de novo dentro deste tempo (qualquer outro comando no meio cancela).
const uint32_t LIGA_CONFIRMA_MS = 5000;

#if PAINEL_MANUAL
// =====================================================================
//  POTENCIOMETROS E BOTOES (so com PAINEL_MANUAL 1)
// =====================================================================
const uint32_t POT_PERIODO_MS   = 20;
const float    POT_FILTRO       = 0.2f;   // media movel exponencial (constante de tempo ~0,1 s)
const float    POT_CRU_MIN      = 80.0f;  // leitura do ADC (0..4095) tratada como "inicio" (0 %)
const float    POT_CRU_MAX      = 4000.0f;// leitura tratada como "fim" (100 %)
const float    POT_ZONA_SEGURA  = 0.02f;  // ate 2 % do curso conta como "no zero" (trava do LIGA)
const float    POT_PASSO_D      = 0.005f; // D anda de 0,005 em 0,005
const float    POT_PASSO_ALFA   = 1.0f;   // alfa anda de 1 em 1 grau
const bool     POT3_MUDA_PAGINA = true;   // false se o Pot 3 nao estiver montado
const uint32_t BOTAO_DEBOUNCE_MS = 30;
const uint32_t BOTAO_LONGO_MS    = 800;   // MODO apertado por 0,8 s = proxima pagina da tela
#endif

// =====================================================================
//  TELA OLED
// =====================================================================
#define OLED_SH1106 0                      // 1 se o display for SH1106 (comum nos de 1,3")
const uint32_t TELA_PERIODO_MS  = 250;     // redesenha 4 vezes por segundo
const uint8_t  TELA_NUM_PAGINAS = 3;
const uint32_t TELA_TROCA_MS    = 5000;    // troca automatica de pagina a cada 5 s ("tela auto")
const uint32_t TELA_MSG_MS      = 3000;    // tempo das mensagens (LIGA recusado, protecao...)

// =====================================================================
//  MOTOR DE DISPARO DOS SCRs
// =====================================================================
const float    VM_PICO          = 42.43f;  // pico do secundario: 30 Vrms x raiz(2)
const int32_t  SEMI_NOMINAL_US  = 8333;    // T/2 em 60 Hz
const int32_t  SEMI_MIN_US      = 7692;    // T/2 em 65 Hz (aceita de 45 a 65 Hz)
const int32_t  SEMI_MAX_US      = 11111;   // T/2 em 45 Hz
const uint32_t SUBIDA_MIN_US    = 7000;    // subida de ZC antes de 7 ms da anterior = ruido
const uint32_t W_MIN_US         = 50;      // "pulso" de ZC menor que isso = repique da borda
const uint32_t W_MAX_US         = 2500;    // pulso de ZC maior que isso = sinal errado
const uint8_t  N_SINCRONISMO    = 10;      // semiciclos bons seguidos para "sincronizado"
const uint8_t  N_ALTERNANCIAS   = 4;       // trocas seguidas de POL para "POL OK"
const uint32_t PULSO_MIN_US     = 100;     // limites da largura do pulso de gate
const uint32_t PULSO_MAX_US     = 2000;
const uint32_t PULSO_PADRAO_US  = 500;
const uint32_t MARGEM_FIM_US    = 100;     // o pulso de gate acaba >= 100 us antes da proxima subida do ZC
const uint32_t DISPARO_MIN_US   = 30;      // menor atraso que o timer agenda (folga para o codigo da ISR)
const int32_t  ALFA_MAX_CG      = 18000;   // 180,00 graus (cg = centesimos de grau)
const int32_t  ALFA_SEM_DISPARO = 17900;   // 179 graus ou mais: nao dispara
const int32_t  ALFA_PADRAO_CG   = 18000;   // na bancada o alfa comeca em 180 graus (sem conducao)
const int32_t  RAMPA_PADRAO_CG  = 100;     // 1 grau por semiciclo
const int32_t  RAMPA_MAX_CG     = 3000;    // 30 graus por semiciclo
const uint32_t ZC_TIMEOUT_MS    = 50;      // sem pulso valido de ZC por mais que isso -> desliga
const uint32_t SINC_TIMEOUT_MS  = 250;     // ligado e sem sincronismo por mais que isso -> desliga
const uint32_t AVISO_ESTAVEL_MS = 300;     // so avisa mudanca de estado que dure isso
const uint64_t ALARME_LONGE     = 0xFFFFFF00ULL; // alarme "estacionado" (~71 min): na pratica, nenhum alarme
const uint32_t TOLERANCIA_US    = 3;
