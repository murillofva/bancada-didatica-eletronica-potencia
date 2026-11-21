/*
  disparo_scr.cpp - Disparo de SCRs sincronizado com a rede (bancada didatica)

  Tres interrupcoes (isrZC, isrTimerGate, isrTimerPol), com as funcoes
  inicioDeSemiciclo/fimDoPulsoZC, fazem o trabalho de tempo real. Pinos e
  constantes vem do config.h; os comandos da serial ficam em comandos.cpp e
  controle.cpp. As mensagens "de rotina" do vigia so aparecem nos modos de
  retificador.

  ------------------------------------------------------------------------
  COMO O TEMPO E MEDIDO

  - Um timer de hardware (timerGate, 1 MHz = conta em microssegundos) e
    ZERADO em toda borda de SUBIDA do pulso de ZC. Assim, o valor do contador
    e sempre "quantos us se passaram desde o inicio do pulso de ZC".
  - Na borda de DESCIDA do pulso de ZC, o contador da direto a largura W.
  - O zero verdadeiro da tensao e o CENTRO do pulso: subida + W/2.
  - O meio periodo T/2 (8333 us em 60 Hz) e medido de centro a centro de
    pulsos seguidos e filtrado (media movel exponencial).
  - alfa vira tempo assim:   atraso desde o zero = alfa/180 x T/2.
    Como para alfa pequeno o disparo pode ter que acontecer ANTES da descida
    do pulso de ZC, o disparo e agendado a partir da SUBIDA, usando o W
    medido nos pulsos anteriores:
                             atraso desde a subida = W/2 + alfa/180 x T/2.
  - O alarme do timer liga o gate na hora certa (na interrupcao do timer,
    sem delay() e sem espera ocupada) e um segundo alarme desliga o gate
    depois da largura do pulso.
  ------------------------------------------------------------------------
*/
#include <Arduino.h>
#include <math.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "config.h"
#include "disparo_scr.h"
#include "comandos.h"

// Os GPIOs 32 em diante ficam no "banco 1" de registradores (bit = pino - 32); o config.h garante isso.
// Dentro das interrupcoes escrevemos/lemos direto nesses registradores: faz o
// mesmo que digitalWrite/digitalRead, mas gasta poucos ciclos de clock e sempre
// o mesmo tempo (menos variacao no instante do disparo).
static const uint32_t BIT_GATE_A = 1UL << (PINO_GATE_A - 32);
static const uint32_t BIT_GATE_B = 1UL << (PINO_GATE_B - 32);
static const uint32_t BIT_ZC     = 1UL << (PINO_ZC - 32);
static const uint32_t BIT_POL    = 1UL << (PINO_POL - 32);

// Polaridade de um semiciclo (sinal da tensao do terminal L em relacao ao N)
static const int8_t POL_DESCONHECIDA = 0;
static const int8_t POL_POSITIVA     = 1;
static const int8_t POL_NEGATIVA     = -1;

// Estados do pulso de gate (maquina de estados do timerGate)
static const uint8_t GATE_OCIOSO     = 0;  // nada agendado
static const uint8_t GATE_AGUARDANDO = 1;  // disparo agendado, esperando o alarme
static const uint8_t GATE_PULSO      = 2;  // gate em HIGH, esperando o alarme de fim

// ============================== Timers =============================
static hw_timer_t *timerGate = nullptr;  // zerado em cada subida do ZC; agenda inicio/fim do pulso de gate
static hw_timer_t *timerPol  = nullptr;  // agenda a leitura do sinal POL no meio de cada semiciclo
static bool timersOk = false;

// Trava (spinlock) que protege as variaveis divididas entre as interrupcoes e o loop().
// No loop(), portENTER_CRITICAL desliga as interrupcoes deste nucleo por alguns
// microssegundos enquanto copiamos ou alteramos varias variaveis "de uma vez".
static portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;

// ---- Ajustes (o loop escreve, as interrupcoes leem) ----
// "volatile" avisa o compilador que o valor pode mudar a qualquer momento
// (por uma interrupcao), entao ele sempre le da memoria.
static volatile bool     armado       = false;            // disparo ligado
static volatile uint8_t  modo         = 1;                // 1 = meia onda, 2 = ponte semicontrolada
static volatile int32_t  alfaAlvo_cg  = ALFA_PADRAO_CG;   // alfa pedido (centesimos de grau)
static volatile int32_t  alfaAtual_cg = ALFA_MAX_CG;      // alfa usado agora (anda em rampa ate o alvo)
static volatile int32_t  rampa_cg     = RAMPA_PADRAO_CG;  // passo da rampa por semiciclo
static volatile uint32_t pulsoGate_us = PULSO_PADRAO_US;

// ---- Medidas (as interrupcoes escrevem, o loop le) ----
static volatile int32_t  semi16           = SEMI_NOMINAL_US * 16;  // T/2 filtrado, em us x 16 (4 bits de fracao)
static volatile int32_t  larg16           = 600 * 16;              // W filtrado, em us x 16
static volatile uint8_t  semiciclosBons   = 0;   // semiciclos validos seguidos
static volatile uint32_t pulsosValidos    = 0;   // pulsos de ZC com largura valida (total)
static volatile uint32_t pulsosRejeitados = 0;   // pulsos/intervalos fora da faixa (total)
static volatile uint32_t disparosA        = 0;   // semiciclos em que o GATE_A deu pulso
static volatile uint32_t disparosB        = 0;   // semiciclos em que o GATE_B deu pulso
static volatile bool     polOk            = false;
static volatile int8_t   polSemiciclo     = POL_DESCONHECIDA;  // polaridade do semiciclo atual
static volatile uint32_t ultimoAtraso_us  = 0;   // ultimo disparo: atraso desde a subida do ZC
static volatile uint32_t ultimaLargura_us = 0;   // ultimo disparo: largura efetiva do pulso

// ---- Estado interno das interrupcoes ----
static volatile bool     zcEmPulso           = false;  // estamos dentro de um pulso de ZC?
static volatile bool     subidaOk            = false;  // intervalo entre as 2 ultimas subidas e plausivel?
static volatile bool     pulsoAnteriorValido = false;
static volatile uint32_t intervaloSubidas    = 0;      // us entre as 2 ultimas subidas
static volatile uint32_t wAnterior           = 0;      // largura do pulso anterior (us)
static volatile bool     amostraPolNova      = false;  // ha leitura de POL feita neste semiciclo?
static volatile bool     amostraPolAlta      = false;  // valor lido (true = HIGH = semiciclo negativo)
static volatile uint32_t atrasoPol_us        = 0;      // quando a leitura de POL foi agendada
static volatile int8_t   polTerminouAntes    = POL_DESCONHECIDA;
static volatile uint8_t  alternancias        = 0;

static volatile uint8_t  estadoGate       = GATE_OCIOSO;
static volatile uint32_t mascaraAgendada  = 0;   // quais gates ligar (BIT_GATE_A e/ou BIT_GATE_B)
static volatile uint32_t instanteDisparo  = 0;   // us desde a subida do ZC
static volatile uint32_t instanteFim      = 0;   // us desde a subida do ZC

struct Instantaneo {  // copia das variaveis compartilhadas (ver lerInstantaneo)
  bool armado, sincronizado, polOk;
  uint8_t modo;
  int8_t polSemiciclo;
  int32_t alfaAlvo, alfaAtual, rampa, semi16, larg16;
  uint32_t pulso, validos, rejeitados, dispA, dispB, ultimoAtraso, ultimaLargura;
};

struct Aviso {        // ver mudouEstavel
  int8_t relatado;    // -1 = ainda nao avisou; 0 = avisou "nao OK"; 1 = avisou "OK"
  bool atual;
  uint32_t desdeMs;
};

// ======================= Funcoes rapidas de pino ===================
// "always_inline": o codigo e copiado dentro da interrupcao (que fica na IRAM).
static inline __attribute__((always_inline)) void gatesLiga(uint32_t mascara) {
  REG_WRITE(GPIO_OUT1_W1TS_REG, mascara);  // W1TS = "escreve 1 para SETAR" (HIGH)
}
static inline __attribute__((always_inline)) void gatesDesliga() {
  REG_WRITE(GPIO_OUT1_W1TC_REG, BIT_GATE_A | BIT_GATE_B);  // W1TC = "escreve 1 para LIMPAR" (LOW)
}
static inline __attribute__((always_inline)) bool lePino(uint32_t bit) {
  return (REG_READ(GPIO_IN1_REG) & bit) != 0;
}
static inline __attribute__((always_inline)) uint32_t limita32(uint64_t v) {
  return (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)v;
}

// ====================================================================
//                         INTERRUPCOES (ISRs)
// Regras: curtas, sem delay(), sem Serial, sem espera ocupada, sem float.
// As tres ISRs rodam no mesmo nucleo e na mesma prioridade, entao uma nunca
// interrompe a outra no meio; a trava protege contra o loop().
// ====================================================================

// ---------------------------------------------------------------------
// Inicio de um semiciclo: chamada na borda de SUBIDA do pulso de ZC
// (W/2 antes do zero). "intervalo" = us desde a subida anterior.
// ---------------------------------------------------------------------
static void IRAM_ATTR inicioDeSemiciclo(uint32_t intervalo) {
  // 1) Seguranca: um semiciclo terminou. Nenhum pulso de gate passa para o
  //    proximo semiciclo, e qualquer disparo antigo e cancelado.
  gatesDesliga();
  estadoGate = GATE_OCIOSO;

  // 2) O intervalo entre subidas e plausivel (45 a 65 Hz, com folga de
  //    500 us porque as subidas variam um pouco com W) e o pulso anterior
  //    foi bom? Se nao, este semiciclo nao dispara.
  bool ok = pulsoAnteriorValido &&
            intervalo >= (uint32_t)(SEMI_MIN_US - 500) &&
            intervalo <= (uint32_t)(SEMI_MAX_US + 500);
  subidaOk = ok;
  intervaloSubidas = intervalo;

  // 3) Polaridade do semiciclo que esta COMECANDO.
  //    O sinal POL foi lido no MEIO do semiciclo que esta terminando (perto
  //    do pico da senoide), onde ele e firme. Perto do zero ele nao e
  //    confiavel: os LEDs dos optoacopladores quase nao tem corrente e os
  //    fototransistores com 10 kOhm sao lentos. Como os semiciclos sempre
  //    se alternam, o que comeca agora tem a polaridade OPOSTA a do que
  //    acabou de terminar.
  int8_t terminou = POL_DESCONHECIDA;
  if (ok && amostraPolNova) terminou = amostraPolAlta ? POL_NEGATIVA : POL_POSITIVA;
  amostraPolNova = false;

  // Conferencia: as leituras precisam se alternar (+, -, +, -...).
  // Se o POL estiver desligado ou parado, nao alterna -> polOk = false.
  if (terminou != POL_DESCONHECIDA && polTerminouAntes != POL_DESCONHECIDA &&
      terminou != polTerminouAntes) {
    if (alternancias < 255) alternancias = alternancias + 1;
  } else {
    alternancias = 0;
  }
  polTerminouAntes = terminou;
  polOk = (alternancias >= N_ALTERNANCIAS);
  int8_t comecando = (int8_t)(-terminou);  // positivo <-> negativo (desconhecida continua 0)
  polSemiciclo = comecando;

  // 4) Pode disparar neste semiciclo?
  bool sincronizado = (semiciclosBons >= N_SINCRONISMO);
  bool podeDisparar = armado && ok && sincronizado && (modo == 2 || polOk);
  if (!podeDisparar) {
    // Se estava ligado, alfa volta para 180: quando puder disparar de novo,
    // a partida e suave (rampa), e nao um "degrau" de potencia.
    if (armado) alfaAtual_cg = ALFA_MAX_CG;
    timerAlarm(timerGate, ALARME_LONGE, false, 0);
    return;
  }

  // 5) Rampa: alfa anda no maximo "rampa_cg" por semiciclo ate o alvo.
  //    (Por isso uma mudanca de alfa so vale a partir do proximo semiciclo.)
  int32_t alvo = alfaAlvo_cg;
  int32_t atual = alfaAtual_cg;
  int32_t passo = rampa_cg;
  if (passo <= 0) {
    atual = alvo;
  } else if (atual < alvo) {
    atual = atual + passo;
    if (atual > alvo) atual = alvo;
  } else if (atual > alvo) {
    atual = atual - passo;
    if (atual < alvo) atual = alvo;
  }
  alfaAtual_cg = atual;

  // 6) Qual gate dispara neste semiciclo?
  uint32_t mascara = 0;
  if (modo == 1) {
    // Meia onda: so o SCR A, so no semiciclo positivo. Sem POL confiavel, nao dispara.
    if (polOk && comecando == POL_POSITIVA) mascara = BIT_GATE_A;
  } else {
    if (!polOk) {
      // Modo reserva (POL ausente/parado): dispara os DOIS gates em todo
      // semiciclo. So conduz o SCR que estiver com o anodo positivo; o outro
      // esta reversamente polarizado e o diodo em serie com o gate dele nao
      // deixa passar corrente de gate.
      mascara = BIT_GATE_A | BIT_GATE_B;
    } else if (comecando == POL_POSITIVA) {
      mascara = BIT_GATE_A;
    } else if (comecando == POL_NEGATIVA) {
      mascara = BIT_GATE_B;
    }
  }
  if (mascara == 0 || atual >= ALFA_SEM_DISPARO) {
    timerAlarm(timerGate, ALARME_LONGE, false, 0);
    return;
  }

  // 7) Converte alfa em tempo (so contas inteiras dentro da ISR):
  //      tAlfa   = alfa/180 x T/2      (alfa em cg, T/2 em us x 16)
  //      tDisparo = W/2 + tAlfa        (contado a partir da SUBIDA do ZC)
  //    Maior produto: 17900 x 177776 = 3,2e9 < 4,29e9 -> cabe em 32 bits.
  uint32_t semi = (uint32_t)(semi16 >> 4);                          // T/2 em us
  uint32_t tAlfa = ((uint32_t)atual * (uint32_t)semi16) / 288000UL;  // 288000 = 18000 cg x 16
  uint32_t tDisparo = (uint32_t)(larg16 >> 5) + tAlfa;               // larg16 >> 5 = W/2 em us
  if (tDisparo < DISPARO_MIN_US) tDisparo = DISPARO_MIN_US;

  // 8) O pulso de gate tem que terminar antes do fim do semiciclo: limite =
  //    MARGEM_FIM_US antes da proxima subida do ZC (prevista para daqui a T/2).
  //    Se nao couber nem um pulso minimo, este semiciclo nao dispara
  //    (acontece so com alfa perto de 180 graus).
  uint32_t limite = semi - MARGEM_FIM_US;
  if (tDisparo + PULSO_MIN_US > limite) {
    timerAlarm(timerGate, ALARME_LONGE, false, 0);
    return;
  }
  uint32_t largura = pulsoGate_us;
  if (tDisparo + largura > limite) largura = limite - tDisparo;

  // 9) Agenda o disparo no timer (que acabou de ser zerado na subida do ZC).
  instanteDisparo = tDisparo;
  instanteFim = tDisparo + largura;
  mascaraAgendada = mascara;
  ultimoAtraso_us = tDisparo;
  ultimaLargura_us = largura;
  estadoGate = GATE_AGUARDANDO;
  timerAlarm(timerGate, tDisparo, false, 0);
}

// ---------------------------------------------------------------------
// Fim do pulso de ZC: borda de DESCIDA (W/2 depois do zero). "w" = largura.
// ---------------------------------------------------------------------
static void IRAM_ATTR fimDoPulsoZC(uint32_t w) {
  if (w > W_MAX_US) {
    // Pulso comprido demais (ex.: transformador desligado deixa o pino de ZC em HIGH).
    pulsoAnteriorValido = false;
    semiciclosBons = 0;
    pulsosRejeitados = pulsosRejeitados + 1;
    return;
  }
  pulsosValidos = pulsosValidos + 1;

  if (subidaOk) {
    // Distancia de CENTRO a CENTRO entre este pulso e o anterior:
    //   centro = subida + W/2  ->  meio periodo = intervalo entre subidas + (W - Wanterior)/2
    int32_t meio = (int32_t)intervaloSubidas + ((int32_t)w - (int32_t)wAnterior) / 2;
    if (meio >= SEMI_MIN_US && meio <= SEMI_MAX_US) {
      if (semiciclosBons == 0) {
        // Primeiro semiciclo bom: comeca o filtro ja com o valor medido.
        semi16 = meio * 16;
        larg16 = (int32_t)w * 16;
      } else {
        // Filtro (media movel exponencial): anda 1/8 da diferenca por semiciclo.
        semi16 = semi16 + (meio * 16 - semi16) / 8;
        larg16 = larg16 + ((int32_t)w * 16 - larg16) / 8;
      }
      if (semiciclosBons < 255) semiciclosBons = semiciclosBons + 1;
    } else {
      semiciclosBons = 0;  // frequencia fora de 45-65 Hz (ou pulso perdido)
      pulsosRejeitados = pulsosRejeitados + 1;
    }
  } else {
    // Primeiro pulso depois de uma falha: so guarda a largura.
    semiciclosBons = 0;
    larg16 = (int32_t)w * 16;
  }
  wAnterior = w;
  pulsoAnteriorValido = true;

  // Agenda a leitura do POL para o MEIO deste semiciclo (zero + T/4 = pico):
  // a partir da descida faltam (T/2)/2 - W/2.
  int32_t semi = (semiciclosBons > 0) ? (semi16 >> 4) : SEMI_NOMINAL_US;
  int32_t atraso = semi / 2 - (int32_t)(w / 2);
  if (atraso < 100) atraso = 100;
  atrasoPol_us = (uint32_t)atraso;
  timerWrite(timerPol, 0);
  timerAlarm(timerPol, (uint64_t)atraso, false, 0);
}

// ---------------------------------------------------------------------
// ISR do pino de ZC (GPIO41), nas duas bordas (CHANGE).
// ---------------------------------------------------------------------
static void IRAM_ATTR isrZC() {
  // Primeiro de tudo: o "carimbo de tempo" e o nivel do pino.
  // O timerGate conta us desde a ultima subida aceita.
  uint32_t t = limita32(timerRead(timerGate));
  bool alto = lePino(BIT_ZC);

  portENTER_CRITICAL_ISR(&trava);
  if (alto && !zcEmPulso) {
    // Borda de SUBIDA. Repiques e ruido logo depois da subida anterior
    // (menos de 7 ms) sao ignorados.
    if (t >= SUBIDA_MIN_US) {
      timerWrite(timerGate, 0);  // nova referencia de tempo: inicio deste pulso
      zcEmPulso = true;
      inicioDeSemiciclo(t);
    }
  } else if (!alto && zcEmPulso) {
    // Borda de DESCIDA. Como o timer foi zerado na subida, t = W.
    // Um "pulso" de menos de 50 us e repique da propria subida (a borda do
    // fototransistor e lenta e o pino pode "tremer" perto do limiar): ignora.
    if (t >= W_MIN_US) {
      zcEmPulso = false;
      fimDoPulsoZC(t);
    }
  }
  // Outros casos (nivel igual ao estado atual) sao repiques: ignora.
  portEXIT_CRITICAL_ISR(&trava);
}

// ---------------------------------------------------------------------
// ISR do timerGate: liga o gate (alarme de disparo) e depois desliga
// (alarme de fim do pulso).
// ---------------------------------------------------------------------
static void IRAM_ATTR isrTimerGate() {
  uint32_t t = limita32(timerRead(timerGate));  // us desde a subida do ZC

  portENTER_CRITICAL_ISR(&trava);
  if (estadoGate == GATE_AGUARDANDO) {
    // Confere se e mesmo o alarme do disparo (e nao uma interrupcao atrasada
    // de um alarme antigo, de antes da ultima subida do ZC).
    if (t + TOLERANCIA_US >= instanteDisparo) {
      gatesLiga(mascaraAgendada);
      estadoGate = GATE_PULSO;
      // Fim do pulso em tempo ABSOLUTO (desde a subida do ZC): a largura do
      // pulso fica exata mesmo que esta ISR tenha comecado alguns us atrasada.
      timerAlarm(timerGate, instanteFim, false, 0);
      if (mascaraAgendada & BIT_GATE_A) disparosA = disparosA + 1;
      if (mascaraAgendada & BIT_GATE_B) disparosB = disparosB + 1;
    }
  } else {
    // Fim do pulso (ou qualquer alarme inesperado): gates em LOW.
    gatesDesliga();
    estadoGate = GATE_OCIOSO;
  }
  portEXIT_CRITICAL_ISR(&trava);
}

// ---------------------------------------------------------------------
// ISR do timerPol: le o sinal POL no meio do semiciclo.
// ---------------------------------------------------------------------
static void IRAM_ATTR isrTimerPol() {
  uint32_t t = limita32(timerRead(timerPol));
  bool alto = lePino(BIT_POL);

  portENTER_CRITICAL_ISR(&trava);
  // So vale se estamos fora do pulso de ZC e se e o alarme agendado agora.
  if (!zcEmPulso && t + TOLERANCIA_US >= atrasoPol_us) {
    amostraPolAlta = alto;
    amostraPolNova = true;
  }
  portEXIT_CRITICAL_ISR(&trava);
}

// ====================================================================
//                       PARTE "LENTA" (loop)
// ====================================================================

// Copia das variaveis compartilhadas, tirada de uma vez (dentro da trava),
// para que os numeros mostrados sejam todos do mesmo instante.
static Instantaneo lerInstantaneo() {
  Instantaneo s;
  portENTER_CRITICAL(&trava);
  s.armado        = armado;
  s.sincronizado  = (semiciclosBons >= N_SINCRONISMO);
  s.polOk         = polOk;
  s.modo          = modo;
  s.polSemiciclo  = polSemiciclo;
  s.alfaAlvo      = alfaAlvo_cg;
  s.alfaAtual     = alfaAtual_cg;
  s.rampa         = rampa_cg;
  s.semi16        = semi16;
  s.larg16        = larg16;
  s.pulso         = pulsoGate_us;
  s.validos       = pulsosValidos;
  s.rejeitados    = pulsosRejeitados;
  s.dispA         = disparosA;
  s.dispB         = disparosB;
  s.ultimoAtraso  = ultimoAtraso_us;
  s.ultimaLargura = ultimaLargura_us;
  portEXIT_CRITICAL(&trava);
  return s;
}

// Estado so do loop
static bool        zcPresente     = false;  // pulsos de ZC validos chegando?
static bool        avisouSemZc    = false;
static uint32_t    contadorVisto  = 0;
static uint32_t    ultimoPulsoMs  = 0;
static uint32_t    ultimoSincMs   = 0;
static bool        avisarFimRampa = false;
static const char *motivoDesligou = nullptr;  // para a tela (desligamento automatico)

// Aviso de mudanca de estado so depois que ela fica estavel por AVISO_ESTAVEL_MS.
static Aviso avisoSinc = { -1, false, 0 };
static Aviso avisoPol  = { -1, false, 0 };

static bool mudouEstavel(Aviso &a, bool ok, uint32_t agoraMs) {
  if (ok != a.atual) {
    a.atual = ok;
    a.desdeMs = agoraMs;
  }
  int8_t valor = a.atual ? 1 : 0;
  if (valor != a.relatado && agoraMs - a.desdeMs >= AVISO_ESTAVEL_MS) {
    a.relatado = valor;
    return true;
  }
  return false;
}

// Desliga o disparo na hora: gates em LOW e nada agendado.
static void desligarDisparo() {
  portENTER_CRITICAL(&trava);
  armado = false;
  estadoGate = GATE_OCIOSO;  // se um alarme ainda chegar, a ISR so poe os gates em LOW
  gatesDesliga();
  alfaAtual_cg = ALFA_MAX_CG;
  portEXIT_CRITICAL(&trava);
  avisarFimRampa = false;
}

// Esquece o sincronismo (usado quando o sinal de ZC some).
static void perderSincronismo() {
  portENTER_CRITICAL(&trava);
  semiciclosBons = 0;
  pulsoAnteriorValido = false;
  alternancias = 0;
  polOk = false;
  polSemiciclo = POL_DESCONHECIDA;
  polTerminouAntes = POL_DESCONHECIDA;
  portEXIT_CRITICAL(&trava);
}

// Vo media teorica, carga resistiva, componentes ideais.
static float voTeorica(uint8_t m, float alfaGraus) {
  if (alfaGraus >= ALFA_SEM_DISPARO / 100.0f) return 0.0f;
  float a = alfaGraus * (float)PI / 180.0f;
  float k = (m == 1) ? 1.0f / (2.0f * (float)PI) : 1.0f / (float)PI;
  return VM_PICO * k * (1.0f + cosf(a));
}

// "assincrono" = mensagem que aparece sozinha (nao e resposta a um comando):
// com o log ligado, ela sai com "# " na frente (ver aviso() em comandos.cpp).
static Print &saida(bool assincrono) {
  if (assincrono) return aviso();
  return con();
}

static void avisoPolDoModo(bool assincrono, uint8_t m) {
  if (m == 1) {
    saida(assincrono).println("  Meia onda: SEM o sinal POL o programa NAO dispara (nao sabe qual e o semiciclo positivo).");
  } else {
    saida(assincrono).println("  Ponte: modo reserva -> GATE_A e GATE_B disparam em TODO semiciclo.");
    saida(assincrono).println("  So conduz o SCR com anodo positivo; o outro fica protegido pelo diodo do gate.");
  }
}

// ====================================================================
//                         FUNCOES PUBLICAS
// ====================================================================

void scrIniciar() {
  // Primeiro de tudo: saidas de gate em LOW (SCRs sem disparo).
  pinMode(PINO_GATE_A, OUTPUT);
  pinMode(PINO_GATE_B, OUTPUT);
  digitalWrite(PINO_GATE_A, LOW);
  digitalWrite(PINO_GATE_B, LOW);
  // Pull-up interno (~45 kOhm) alem dos 10 kOhm externos. Sem nada ligado, o pino fica em
  // HIGH em vez de flutuar e gerar "pulsos de ZC" falsos.
  pinMode(PINO_ZC, INPUT_PULLUP);
  pinMode(PINO_POL, INPUT_PULLUP);

  // Dois timers de 1 MHz (1 contagem = 1 us).
  timerGate = timerBegin(1000000);
  timerPol = timerBegin(1000000);
  if (timerGate == nullptr || timerPol == nullptr) {
    // So o disparo fica indisponivel (gates em LOW); os outros modos
    // continuam funcionando.
    con().println("ERRO ao criar os timers! O disparo dos SCRs fica INDISPONIVEL.");
    timersOk = false;
    return;
  }
  timerAttachInterrupt(timerGate, &isrTimerGate);
  timerAttachInterrupt(timerPol, &isrTimerPol);
  timerAlarm(timerGate, ALARME_LONGE, false, 0);  // nenhum alarme por enquanto
  timerAlarm(timerPol, ALARME_LONGE, false, 0);

  attachInterrupt(digitalPinToInterrupt(PINO_ZC), isrZC, CHANGE);
  ultimoPulsoMs = millis();  // conta os 50 ms do vigia a partir de agora
  timersOk = true;
}

bool scrDisponivel() {
  return timersOk;
}

void scrDesligar() {
  desligarDisparo();
}

// (Pode ser chamada pelo botao LIGA: as mensagens saem por aviso().)
bool scrLigar() {
  if (!timersOk) {
    aviso().println("NAO liguei: os timers do disparo nao foram criados (veja a mensagem do inicio).");
    return false;
  }
  Instantaneo s = lerInstantaneo();
  if (!zcPresente || !s.sincronizado) {
    aviso().println("NAO liguei: sem sinal de ZC valido (precisa de pulsos de 45 a 65 Hz no " TXT_ZC "). Veja \"?\".");
    return false;
  }
  if (s.modo == 1 && !s.polOk) {
    aviso().println("NAO liguei: a meia onda precisa do sinal POL (" TXT_POL ") funcionando para achar o semiciclo positivo.");
    return false;
  }
  portENTER_CRITICAL(&trava);
  alfaAtual_cg = ALFA_MAX_CG;  // partida suave: comeca em 180 graus (sem conducao) e desce em rampa
  disparosA = 0;
  disparosB = 0;
  armado = true;
  portEXIT_CRITICAL(&trava);
  avisarFimRampa = false;
  motivoDesligou = nullptr;

  comTempo(aviso()).printf("Disparo LIGADO (%s). alfa comeca em 180 graus e vai ate o alvo (%.1f graus), rampa de %.1f grau/semiciclo.\n",
                 s.modo == 1 ? "meia onda" : "ponte semicontrolada", s.alfaAlvo / 100.0f, s.rampa / 100.0f);
  if (s.modo == 2 && !s.polOk) {
    aviso().println("ATENCAO: sinal POL nao esta OK.");
    avisoPolDoModo(true, 2);
  }
  return true;
}

void scrDefinirModo(uint8_t m) {
  desligarDisparo();
  modo = (m == 1) ? 1 : 2;
}

void scrDefinirAlfa(float graus, bool avisar) {
  if (graus < 0.0f) graus = 0.0f;
  if (graus > 180.0f) graus = 180.0f;
  alfaAlvo_cg = (int32_t)lroundf(graus * 100.0f);
  if (avisar && armado) avisarFimRampa = true;
}

void scrDefinirPulso(uint32_t us) {
  if (us < PULSO_MIN_US) us = PULSO_MIN_US;
  if (us > PULSO_MAX_US) us = PULSO_MAX_US;
  pulsoGate_us = us;
}

void scrDefinirRampa(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > RAMPA_MAX_CG / 100.0f) v = RAMPA_MAX_CG / 100.0f;
  rampa_cg = (int32_t)lroundf(v * 100.0f);
}

float scrRampa() {
  return rampa_cg / 100.0f;
}

ScrResumo scrResumo() {
  Instantaneo s = lerInstantaneo();
  ScrResumo r;
  r.armado = s.armado;
  r.zcPresente = zcPresente;
  r.sincronizado = zcPresente && s.sincronizado;
  r.polOk = s.polOk;
  r.modo = s.modo;
  r.alfaAlvo = s.alfaAlvo / 100.0f;
  r.alfaAtual = s.alfaAtual / 100.0f;
  r.freqRede = r.sincronizado ? 1e6f / (2.0f * (s.semi16 / 16.0f)) : 0.0f;
  r.pulso_us = s.pulso;
  r.semi_us = s.semi16 / 16.0f;  // como no "?" (scrMostrarEstado)
  r.w_us = s.larg16 / 16.0f;
  r.dispA = s.dispA;
  r.dispB = s.dispB;
  return r;
}

float scrVoMedTeorica(uint8_t m, float alfaGraus) {
  return voTeorica(m, alfaGraus);
}

// RMS teorico (carga R, ideal): meia onda Vm/2 x k ; ponte Vm/raiz(2) x k,
// com k = raiz(1 - a/pi + sen(2a)/(2 pi)). O "?" e a pagina usam esta mesma conta.
float scrVoRmsTeorica(uint8_t m, float alfaGraus) {
  float ar = alfaGraus * (float)PI / 180.0f;
  float k = sqrtf(fmaxf(0.0f, 1.0f - ar / (float)PI + sinf(2.0f * ar) / (2.0f * (float)PI)));
  float vrms = (m == 1 ? VM_PICO / 2.0f : VM_PICO / sqrtf(2.0f)) * k;
  if (alfaGraus >= ALFA_SEM_DISPARO / 100.0f) vrms = 0.0f;
  return vrms;
}

// Fase da rede AGORA (para a medicao do ADS1115). O contador do timerGate e
// lido junto com o estado das interrupcoes, dentro da trava, como no
// lerInstantaneo. "meio" vem do contador de pulsos validos: a paridade dele
// separa os dois semiciclos de um ciclo (sem precisar do sinal POL).
FaseRede scrFase() {
  FaseRede f;
  if (!timersOk) {
    f.valida = false;
    f.t_us = 0;
    f.semi_us = SEMI_NOMINAL_US;
    f.w_us = 0;
    f.meio = 0;
    return f;
  }
  portENTER_CRITICAL(&trava);
  uint32_t t = limita32(timerRead(timerGate));
  bool emPulso = zcEmPulso;
  uint32_t validos = pulsosValidos;
  bool sinc = (semiciclosBons >= N_SINCRONISMO);
  int32_t s16 = semi16;
  int32_t l16 = larg16;
  portEXIT_CRITICAL(&trava);

  f.t_us = t;
  f.semi_us = (uint32_t)(s16 >> 4);
  f.w_us = (uint32_t)(l16 >> 4);
  // Dentro do pulso de ZC (entre a subida e a descida) o contador de pulsos
  // validos ainda nao contou o semiciclo que acabou de comecar.
  f.meio = emPulso ? validos + 1 : validos;
  f.valida = zcPresente && sinc && f.semi_us > 0 && t < 2 * f.semi_us;
  return f;
}

const char *scrPegarMotivo() {
  const char *m = motivoDesligou;
  motivoDesligou = nullptr;
  return m;
}

void scrMostrarEstado() {
  Instantaneo s = lerInstantaneo();
  float semi = s.semi16 / 16.0f;  // us
  float w = s.larg16 / 16.0f;     // us
  float freq = 1e6f / (2.0f * semi);
  float alvo = s.alfaAlvo / 100.0f;
  float atual = s.alfaAtual / 100.0f;

  if (!timersOk) con().println("Disparo INDISPONIVEL (timers nao criados).");
  con().printf("Disparo: %s | alfa alvo = %.1f graus | alfa atual = ", s.armado ? "LIGADO" : "DESLIGADO", alvo);
  if (s.armado) con().printf("%.1f graus", atual);
  else con().print("-- (sem disparo)");
  if (s.rampa > 0) con().printf(" | rampa = %.1f grau/semiciclo\n", s.rampa / 100.0f);
  else con().println(" | sem rampa");

  if (!zcPresente) {
    con().println("Rede: SEM SINAL DE ZC no " TXT_ZC " (confira trafo, H11AA1, resistores e pull-up de 10 kOhm)");
  } else {
    con().printf("Rede: %s | f medida = %.2f Hz | T/2 = %.0f us | largura do pulso de ZC (W) = %.0f us\n",
                  s.sincronizado ? "SINCRONIZADO" : "NAO sincronizado", freq, semi, w);
  }

  if (s.polOk) {
    con().printf("Sinal POL (" TXT_POL "): OK, alternando | semiciclo atual: %s\n",
                  s.polSemiciclo == POL_POSITIVA ? "positivo" : (s.polSemiciclo == POL_NEGATIVA ? "negativo" : "?"));
  } else {
    con().println("Sinal POL (" TXT_POL "): NAO OK (parado ou ausente)");
    avisoPolDoModo(false, s.modo);
  }

  // Conversao alfa -> tempo (para o alfa alvo)
  float tZero = alvo / 180.0f * semi;
  float tSubida = w / 2.0f + tZero;
  float alfaMax = 180.0f * (semi - MARGEM_FIM_US - PULSO_MIN_US - w / 2.0f) / semi;
  con().printf("Pulso de gate = %lu us | alfa alvo -> atraso desde o zero = %.0f us, desde a subida do ZC = %.0f us\n",
                (unsigned long)s.pulso, tZero, tSubida);
  con().printf("(1 grau = %.1f us; alfa acima de ~%.0f graus nao dispara: falta tempo antes do fim do semiciclo)\n",
                semi / 180.0f, alfaMax);
  if (s.armado && s.ultimaLargura > 0) {
    con().printf("Ultimo disparo agendado: %lu us apos a subida do ZC, pulso de %lu us\n",
                  (unsigned long)s.ultimoAtraso, (unsigned long)s.ultimaLargura);
  }

  con().printf("Semiciclos disparados: A = %lu | B = %lu | pulsos de ZC validos = %lu | rejeitados = %lu\n",
                (unsigned long)s.dispA, (unsigned long)s.dispB, (unsigned long)s.validos, (unsigned long)s.rejeitados);

  const char *formula = (s.modo == 1) ? "Vm/(2*pi)*(1+cos a)" : "Vm/pi*(1+cos a)";
  con().printf("Vo media teorica (carga R, ideal, 30 Vrms, Vm = %.2f V, %s):\n", VM_PICO, formula);
  if (s.armado) {
    con().printf("  alfa atual %.1f -> %.2f V | alfa alvo %.1f -> %.2f V\n",
                  atual, voTeorica(s.modo, atual), alvo, voTeorica(s.modo, alvo));
  } else {
    con().printf("  disparo desligado -> 0 V | com alfa alvo %.1f graus seria %.2f V\n", alvo, voTeorica(s.modo, alvo));
  }
}

// Vigia os sinais de ZC e POL e desliga o disparo se algo der errado.
// (Mesmo sem o loop, as ISRs nunca disparam sem um pulso de ZC valido.)
// "mensagens" = false nos modos dos conversores: o vigia continua igual,
// so as mensagens de rotina ficam caladas.
void scrVerificar(bool mensagens) {
  if (!timersOk) return;
  uint32_t agora = millis();

  // 1) Chegam pulsos validos de ZC?
  uint32_t c = pulsosValidos;
  if (c != contadorVisto) {
    contadorVisto = c;
    ultimoPulsoMs = agora;
    if (!zcPresente) {
      zcPresente = true;
      avisouSemZc = false;
      if (mensagens) comTempo(aviso()).println("Pulsos de ZC chegando no " TXT_ZC ". Medindo a frequencia...");
    }
  } else if (agora - ultimoPulsoMs > ZC_TIMEOUT_MS && (zcPresente || !avisouSemZc)) {
    bool estava = armado;
    desligarDisparo();
    perderSincronismo();
    zcPresente = false;
    avisouSemZc = true;
    avisoSinc.relatado = -1;
    avisoPol.relatado = -1;
    if (estava) {
      comTempo(aviso()).println("ATENCAO: nenhum pulso valido de ZC ha mais de 50 ms -> disparo DESLIGADO (gates em LOW).");
      motivoDesligou = "sem pulsos de ZC";
    }
    if (mensagens) comTempo(aviso()).println("Sem sinal de ZC no " TXT_ZC " (trafo desligado? H11AA1 ou pull-up de 10 kOhm faltando?).");
  }

  // 2) Sincronismo (frequencia de 45 a 65 Hz, medida de forma estavel)
  bool sinc = zcPresente && (semiciclosBons >= N_SINCRONISMO);
  if (sinc) ultimoSincMs = agora;
  if (armado && agora - ultimoSincMs > SINC_TIMEOUT_MS) {
    desligarDisparo();
    comTempo(aviso()).println("ATENCAO: sem sincronismo com a rede ha mais de 250 ms -> disparo DESLIGADO (gates em LOW).");
    motivoDesligou = "sem sincronismo";
  }
  if (zcPresente && mudouEstavel(avisoSinc, sinc, agora) && mensagens) {
    if (sinc) {
      float semi = semi16 / 16.0f;
      comTempo(aviso()).printf("Sincronizado com a rede: f = %.2f Hz, T/2 = %.0f us, W = %.0f us.\n",
                     1e6f / (2.0f * semi), semi, larg16 / 16.0f);
    } else {
      comTempo(aviso()).println("Pulsos de ZC presentes, mas SEM sincronismo (frequencia fora de 45-65 Hz ou ruido no " TXT_ZC ").");
    }
  }

  // 3) Sinal POL (so faz sentido avaliar com o ZC sincronizado)
  if (sinc) {
    bool pol = polOk;
    if (mudouEstavel(avisoPol, pol, agora) && mensagens) {
      if (pol) {
        comTempo(aviso()).println("Sinal POL OK (" TXT_POL " alternando a cada semiciclo).");
      } else {
        comTempo(aviso()).println("ATENCAO: sinal POL parado ou ausente no " TXT_POL ".");
        avisoPolDoModo(true, modo);
      }
    }
  }

  // 4) Como no Boost: quando a rampa de alfa (pedida pela serial) chega ao alvo, avisa.
  if (avisarFimRampa) {
    if (!armado) {
      avisarFimRampa = false;
    } else if (alfaAtual_cg == alfaAlvo_cg) {
      avisarFimRampa = false;
      comTempo(aviso()).printf("alfa chegou ao alvo: %.1f graus.\n", alfaAlvo_cg / 100.0f);
    }
  }
}
