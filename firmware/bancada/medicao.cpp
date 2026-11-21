/*
  medicao.cpp - INA226 x2 e ADS1115 pelo I2C (drivers por registrador) e
  calibracoes na NVS.

  ---------------------------------------------------------------------------
  INA226 (conversores)
    Configuracao: media de 16 conversoes, 1,1 ms por conversao (shunt e
    barramento), modo continuo -> um valor novo a cada 35,2 ms.
    - Tensao no shunt: registrador 0x01, com sinal, 2,5 uV por bit
      (fundo de escala +-81,92 mV -> com 0,1 Ohm, +-0,82 A; com o Rsh1 calibrado
      de 0,109 Ohm, +-0,75 A). Registrador no fundo de escala = "saturado": a
      leitura trava e, nos conversores, a saida desliga (controle.cpp).
    - Tensao do barramento (pino VBUS): registrador 0x02, 1,25 mV por bit (max 36 V).
    - Corrente calculada AQUI: I = Vshunt / Rshunt, com Rshunt calibravel
      ("cal rsh1" e "cal rsh2"). O registrador de calibracao do INA226 nao e usado.
    Lidos a cada 50 ms (protecoes) e somados para a media de 1 s.

  ADS1115 (retificadores)
    PGA +-4,096 V (125 uV por bit), 860 amostras/s, conversao unica, alternando
    A0 (tensao, pelo divisor) e A1 (corrente, pelo ACS712): ~250 amostras por
    canal por segundo. Cada conversao e praticamente a MEDIA da entrada
    durante ~1,16 ms (por isso o RMS de ondas com degrau sai um pouco menor:
    ver LEIA-ME).

    Media e RMS em "TEMPO EQUIVALENTE":
    O ADS1115 e lento demais para "desenhar" um ciclo de 16,7 ms (daria so
    ~7 pontos por ciclo por canal). Mas a onda se repete a cada ciclo: se
    cada amostra cair numa FASE diferente do ciclo, 1 s de amostras (60
    ciclos) "desenha" um ciclo completo com ~250 pontos.
      1) Cada amostra e marcada com a fase da rede em que foi feita (0 a 1
         ciclo), usando o instante da ultima passagem por zero que o motor
         dos SCRs ja mede (timer de 1 us).
      2) Antes de cada conversao ha uma espera ALEATORIA de 0 a 0,8 ms. Sem
         ela, se o loop ficar com periodo quase multiplo de 16,7 ms, as
         amostras cairiam sempre nas mesmas poucas fases (erro de ate 30 %
         na simulacao); com ela o erro da media ficou abaixo de 1 %.
      3) No fim da janela as amostras sao ordenadas pela fase e a area sob a
         curva e calculada pela regra dos trapezios:
           Vo_med = integral de v(fase)   e   Vo_rms = raiz(integral de v^2)
         (a fase vai de 0 a 1, entao a integral ja e a media).
    Sem sinal de ZC (rede nao sincronizada), usa a media simples de todas as
    amostras (menos precisa: erro de alguns %).
  ---------------------------------------------------------------------------
*/
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include "config.h"
#include "medicao.h"
#include "disparo_scr.h"
#include "comandos.h"
#include "tela.h"

Calibracao cal;
static bool calAlterada = false;

// ===================================================================
//                          I2C basico
// ===================================================================
static bool i2cEscreve16(uint8_t end, uint8_t reg, uint16_t valor) {
  Wire.beginTransmission(end);
  Wire.write(reg);
  Wire.write((uint8_t)(valor >> 8));
  Wire.write((uint8_t)(valor & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool i2cLe16(uint8_t end, uint8_t reg, uint16_t &valor) {
  Wire.beginTransmission(end);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(end, (size_t)2) != 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t alto = (uint8_t)Wire.read();
  uint8_t baixo = (uint8_t)Wire.read();
  valor = (uint16_t)(((uint16_t)alto << 8) | baixo);
  return true;
}

bool i2cResponde(uint8_t endereco) {
  Wire.beginTransmission(endereco);
  return Wire.endTransmission() == 0;
}

// ===================================================================
//                          Calibracao (NVS)
// ===================================================================
struct LimiteCal {
  const char *chave;  // nome na NVS
  float min, max;     // faixa aceita (para ACS, vale o modulo)
};
static const LimiteCal LIMITES[] = {
  { "rsh1",   0.001f, 10.0f },
  { "rsh2",   0.001f, 10.0f },
  { "div",    1.0f,   100.0f },
  { "acs",    0.01f,  2.0f },
  { "acs0",   1.5f,   3.5f },
  { "rcarga", 0.1f,   100000.0f },
};

static float *campoCal(ItemCal item) {
  switch (item) {
    case CAL_RSH1:     return &cal.rsh1;
    case CAL_RSH2:     return &cal.rsh2;
    case CAL_DIV:      return &cal.div;
    case CAL_ACS:      return &cal.acsSens;
    case CAL_ACS_ZERO: return &cal.acsZero;
    default:           return &cal.rCarga;
  }
}

static bool dentroDaFaixa(ItemCal item, float v) {
  const LimiteCal &l = LIMITES[item];
  float x = (item == CAL_ACS) ? fabsf(v) : v;
  return x >= l.min && x <= l.max;  // NAN da false
}

static void valoresPadrao() {
  cal.rsh1 = RSH1_PADRAO;
  cal.rsh2 = RSH2_PADRAO;
  cal.div = DIV_PADRAO;
  cal.acsSens = ACS_SENS_PADRAO;
  cal.acsZero = ACS_ZERO_PADRAO;
  cal.rCarga = RCARGA_PADRAO;
}

void calPadrao() {
  valoresPadrao();
  calAlterada = true;
}

void calCarregar() {
  valoresPadrao();
  Preferences p;
  if (!p.begin(NVS_NOME, true)) return;  // primeira vez: ainda nao ha nada gravado
  for (uint8_t k = CAL_RSH1; k <= CAL_RCARGA; k++) {
    ItemCal item = (ItemCal)k;
    float *campo = campoCal(item);
    float v = p.getFloat(LIMITES[k].chave, *campo);
    if (dentroDaFaixa(item, v)) *campo = v;
  }
  p.end();
}

bool calDefinir(ItemCal item, float v) {
  if (!dentroDaFaixa(item, v)) return false;
  *campoCal(item) = v;
  calAlterada = true;
  return true;
}

float medicaoFundoEscala(uint8_t k) {
  return INA_FUNDO_ESCALA_V / ((k == 0) ? cal.rsh1 : cal.rsh2);
}

void calMostrarFundoEscala() {
  con().printf("Fundo de escala dos INA226 (81.92 mV / Rsh): %.3f A (#1) | %.3f A (#2). "
               "No Boost e no Buck, um Vshunt no fundo de escala desliga a saida.\n",
               medicaoFundoEscala(0), medicaoFundoEscala(1));
}

void calMostrar() {
  con().printf("Calibracao: Rsh1 = %.4f Ohm | Rsh2 = %.4f Ohm | divisor = %.4f | ACS712 = %.4f V/A, zero = %.4f V | R carga = %.2f Ohm%s\n",
                cal.rsh1, cal.rsh2, cal.div, cal.acsSens, cal.acsZero, cal.rCarga,
                calAlterada ? " (ainda nao gravada: grava com a saida desligada)" : "");
  calMostrarFundoEscala();
}

bool calPendente() {
  return calAlterada;
}

void calGravar() {
  Preferences p;
  if (!p.begin(NVS_NOME, false)) {
    comTempo(aviso()).println("ERRO: nao consegui abrir a memoria (NVS) para gravar a calibracao.");
    calAlterada = false;  // nao fica tentando sem parar
    return;
  }
  for (uint8_t k = CAL_RSH1; k <= CAL_RCARGA; k++) {
    p.putFloat(LIMITES[k].chave, *campoCal((ItemCal)k));
  }
  p.end();
  calAlterada = false;
  comTempo(aviso()).println("Calibracao gravada na memoria do ESP32 (NVS).");
}

// ===================================================================
//                              INA226
// ===================================================================
static const uint8_t END_INA[2] = { END_INA_ENTRADA, END_INA_SAIDA };
static const char *NOME_INA[2] = { "INA226 #1 (0x40, entrada)", "INA226 #2 (0x41, saida)" };

static bool       inaOk[2]     = { false, false };
static uint8_t    inaFalhas[2] = { 0, 0 };
static LeituraIna inaUlt[2];
static bool       inaNova      = false;
static uint32_t   inaContador  = 0;     // quantas rodadas de leitura dos INA226 ja houve (painel web)
static uint32_t   inaUltimaMs  = 0;

struct AcumIna {
  uint32_t n;
  double sv, si, sp, ssh;
};
static AcumIna acIna[2];

// 0 = ok, 1 = nao responde, 2 = responde mas nao e INA226, 3 = configuracao nao confere
static uint8_t inaConfigurar(uint8_t k) {
  uint16_t id = 0, lido = 0;
  if (!i2cLe16(END_INA[k], INA_REG_ID_FAB, id)) return 1;
  if (id != INA_ID_TI) return 2;
  if (!i2cEscreve16(END_INA[k], INA_REG_CONFIG, INA_CONFIG)) return 3;
  if (!i2cLe16(END_INA[k], INA_REG_CONFIG, lido) || lido != INA_CONFIG) return 3;
  return 0;
}

static void inaLer(uint8_t k) {
  if (!inaOk[k]) {
    inaUlt[k].ok = false;
    inaUlt[k].saturado = false;
    return;
  }
  uint16_t rs = 0, rb = 0;
  if (i2cLe16(END_INA[k], INA_REG_SHUNT, rs) && i2cLe16(END_INA[k], INA_REG_BUS, rb)) {
    inaFalhas[k] = 0;
    float rsh = (k == 0) ? cal.rsh1 : cal.rsh2;
    LeituraIna &l = inaUlt[k];
    int16_t cru = (int16_t)rs;
    l.saturado = (cru >= INA_SHUNT_SAT_CRU || cru <= -INA_SHUNT_SAT_CRU);  // no fundo de escala
    l.vShunt = (float)cru * INA_LSB_SHUNT_V;
    l.i = l.vShunt / rsh;
    l.v = (float)rb * INA_LSB_BUS_V;
    l.ok = true;
    AcumIna &a = acIna[k];
    a.n++;
    a.sv += l.v;
    a.si += l.i;
    a.sp += (double)l.v * (double)l.i;
    a.ssh += l.vShunt;
  } else {
    inaFalhas[k]++;
    if (inaFalhas[k] >= INA_FALHAS_MAX) {
      inaOk[k] = false;
      inaUlt[k].ok = false;
      inaUlt[k].saturado = false;
      comTempo(aviso()).printf("ATENCAO: %s parou de responder.\n", NOME_INA[k]);
    }
  }
}

// ===================================================================
//                              ADS1115
// ===================================================================
static bool     adsOk          = false;
static uint8_t  adsFalhas      = 0;
static bool     adsConvertendo = false;
static uint8_t  adsCanal       = 0;     // 0 = A0 (tensao), 1 = A1 (corrente)
static uint32_t adsInicioUs    = 0;
static uint32_t adsProximoUs   = 0;     // proxima conversao (depois da espera aleatoria)
static float    adsFase        = -1.0f; // fase da rede da conversao em andamento (-1 = sem fase)

struct Amostra {
  float fase;   // 0..1 (fracao do ciclo da rede)
  float valor;  // V (canal A0) ou A (canal A1)
};

struct AcumAds {
  uint32_t n, semFase;
  double   s, s2, sCru;  // somas para a media simples e para a calibracao
  float    maxCru;
  uint16_t nGuardadas;
  Amostra  am[ADS_MAX_AMOSTRAS];
};
static AcumAds acAds[2];

static bool adsConfigurar() {
  // Escreve uma configuracao SEM iniciar conversao e confere a leitura: assim
  // sabemos que no 0x48 ha mesmo um ADS1115 (ele nao tem registrador de ID).
  uint16_t cfg = ADS_CFG_BASE | ADS_MUX_AIN0;
  uint16_t lido = 0;
  if (!i2cEscreve16(END_ADS, ADS_REG_CONFIG, cfg)) return false;
  if (!i2cLe16(END_ADS, ADS_REG_CONFIG, lido)) return false;
  return (lido & 0x7FFF) == cfg;
}

// Fase da rede (0 a 1 ciclo) no MEIO da conversao que acabou de comecar.
// Cada ciclo tem dois semiciclos; a paridade do contador de semiciclos diz em
// qual dos dois a amostra caiu (nao precisa saber qual e o positivo: so
// precisa ser sempre o mesmo dentro da janela).
static float faseDaAmostra(const FaseRede &f) {
  if (!f.valida || f.semi_us == 0) return -1.0f;
  int32_t semi = (int32_t)f.semi_us;
  // us desde o ZERO do semiciclo "meio" (o zero fica W/2 depois da subida do pulso de ZC)
  int32_t t = (int32_t)f.t_us + (int32_t)ADS_MEIO_US - (int32_t)(f.w_us / 2);
  uint32_t meio = f.meio;
  if (t < 0) {             // ainda no fim do semiciclo anterior
    t += semi;
    meio -= 1;
  } else if (t >= semi) {  // ja no semiciclo seguinte
    t -= semi;
    meio += 1;
    if (t >= semi) return -1.0f;
  }
  float x = (float)t / (float)semi;  // 0..1 dentro do semiciclo
  return ((meio & 1u) ? 0.5f : 0.0f) + 0.5f * x;
}

static void acumular(AcumAds &a, float valor, float cru, float fase) {
  a.n++;
  a.s += valor;
  a.s2 += (double)valor * (double)valor;
  a.sCru += cru;
  if (a.n == 1 || cru > a.maxCru) a.maxCru = cru;
  if (fase < 0.0f) {
    a.semFase++;
  } else if (a.nGuardadas < ADS_MAX_AMOSTRAS) {
    a.am[a.nGuardadas].fase = fase;
    a.am[a.nGuardadas].valor = valor;
    a.nGuardadas++;
  }
}

static void adsFalhou() {
  adsConvertendo = false;
  adsFalhas++;
  if (adsFalhas >= ADS_FALHAS_MAX) {
    adsOk = false;
    comTempo(aviso()).println("ATENCAO: ADS1115 (0x48) parou de responder.");
  }
}

// Maquina de estados: espera aleatoria, inicia uma conversao, espera ~1,2 ms
// (sem travar o loop), confere o bit OS (conversao pronta) e le o resultado.
static void adsPasso() {
  if (!adsOk) return;
  if (!adsConvertendo) {
    if ((int32_t)(micros() - adsProximoUs) < 0) return;  // ainda na espera aleatoria
    uint16_t cfg = ADS_OS | ADS_CFG_BASE | (adsCanal == 0 ? ADS_MUX_AIN0 : ADS_MUX_AIN1);
    if (!i2cEscreve16(END_ADS, ADS_REG_CONFIG, cfg)) {
      adsFalhou();
      return;
    }
    adsInicioUs = micros();
    adsFase = faseDaAmostra(scrFase());  // fase da rede no instante da conversao
    adsConvertendo = true;
    return;
  }
  uint32_t dt = micros() - adsInicioUs;
  if (dt < ADS_ESPERA_US) return;
  uint16_t cfg = 0;
  if (!i2cLe16(END_ADS, ADS_REG_CONFIG, cfg)) {
    adsFalhou();
    return;
  }
  if ((cfg & ADS_OS) == 0) {  // ainda convertendo
    if (dt > ADS_TIMEOUT_US) adsFalhou();
    return;
  }
  uint16_t bruto = 0;
  if (!i2cLe16(END_ADS, ADS_REG_CONV, bruto)) {
    adsFalhou();
    return;
  }
  adsFalhas = 0;
  adsConvertendo = false;
  float v = (float)(int16_t)bruto * ADS_LSB_V;  // tensao no pino do ADS1115
  if (adsCanal == 0) {
    acumular(acAds[0], v * cal.div, v, adsFase);                    // Vo
  } else {
    acumular(acAds[1], (v - cal.acsZero) / cal.acsSens, v, adsFase);  // Io
  }
  adsCanal = (adsCanal == 0) ? 1 : 0;
  adsProximoUs = micros() + (uint32_t)random((long)ADS_DITHER_US);  // espera aleatoria
}

// Media e RMS de um canal na janela.
// Por fase (tempo equivalente) se todas as amostras tiverem fase conhecida:
// ordena pela fase e integra pela regra dos trapezios ao longo de um ciclo
// (0 a 1, fechando o circulo do ultimo ponto de volta ao primeiro).
// Senao, media simples. "lacuna" = maior buraco entre fases vizinhas (graus).
static void calcular(AcumAds &a, float &media, float &rms, bool &porFase, float &lacuna) {
  porFase = false;
  lacuna = NAN;
  if (a.n == 0) {
    media = NAN;
    rms = NAN;
    return;
  }
  uint16_t n = a.nGuardadas;
  if (a.semFase == 0 && n >= ADS_MIN_AMOSTRAS) {
    std::sort(a.am, a.am + n, [](const Amostra &x, const Amostra &y) { return x.fase < y.fase; });
    double sm = 0.0, sq = 0.0, maior = 0.0;
    for (uint16_t k = 0; k < n; k++) {
      const Amostra &p = a.am[k];
      const Amostra &q = a.am[(k + 1) % n];
      double d = (double)q.fase - (double)p.fase;
      if (k == n - 1) d += 1.0;  // do ultimo ponto (fim do ciclo) ao primeiro (inicio do proximo)
      if (d > maior) maior = d;
      sm += 0.5 * ((double)p.valor + (double)q.valor) * d;
      sq += 0.5 * ((double)p.valor * p.valor + (double)q.valor * q.valor) * d;
    }
    media = (float)sm;
    rms = (float)sqrt(fmax(sq, 0.0));
    lacuna = (float)(maior * 360.0);
    porFase = true;
    return;
  }
  media = (float)(a.s / a.n);
  rms = (float)sqrt(fmax(a.s2 / a.n, 0.0));
}

// ===================================================================
//                         Janela de 1 s
// ===================================================================
static Resultados res;
static uint32_t   janelaInicioMs   = 0;
static uint8_t    zeroEstado       = 0;  // 0 = nada, 1 = esperando a janela, 2 = medindo
static uint32_t   ultimaDeteccaoMs = 0;

static void resultadosVazios(Resultados &r) {
  r.vin = r.iin = r.pin = r.vo = r.io = r.pout = r.eta = NAN;
  r.vShunt1 = r.vShunt2 = NAN;
  r.voMed = r.voRms = r.ioMed = r.ioRms = NAN;
  r.vA0 = r.vA1 = r.vA0max = NAN;
  r.pdc = r.pac = r.etaRet = NAN;
  r.ioRMed = r.ioRRms = NAN;
  r.nA0 = r.nA1 = 0;
  r.porFase = false;
  r.lacunaGraus = NAN;
}

static void terminarZero(const Resultados &r) {
  zeroEstado = 0;
  if (!adsOk || acAds[1].n < 10) {
    comTempo(aviso()).println("Zero do ACS712 FALHOU: sem amostras do ADS1115 no A1.");
    telaMensagem("Zero do ACS712", "FALHOU:", "sem ADS1115");
    return;
  }
  float novo = r.vA1;
  if (!calDefinir(CAL_ACS_ZERO, novo)) {
    comTempo(aviso()).printf("Zero do ACS712 RECUSADO: A1 = %.4f V (esperado perto de 2,5 V). Confira o ACS712 e os 5 V.\n", novo);
    telaMensagem("Zero RECUSADO", "A1 longe de 2,5V", "confira o ACS712");
    return;
  }
  comTempo(aviso()).printf("Zero do ACS712 = %.4f V (sera gravado na memoria).\n", novo);
  if (fabsf(novo - 2.5f) > 0.15f) {
    aviso().println("  Obs.: longe de 2,50 V. Havia corrente na carga? O ACS712 esta com 5 V?");
  }
  char linha[24];
  snprintf(linha, sizeof(linha), "A1 = %.3f V", novo);
  telaMensagem("Zero do ACS712", linha, "gravado");
}

static void fecharJanela() {
  Resultados r;
  resultadosVazios(r);
  r.id = res.id + 1;

  // ---- INA226 ----
  if (acIna[0].n > 0) {
    const AcumIna &a = acIna[0];
    r.vin = (float)(a.sv / a.n);
    r.iin = (float)(a.si / a.n);
    r.pin = (float)(a.sp / a.n);
    r.vShunt1 = (float)(a.ssh / a.n);
  }
  if (acIna[1].n > 0) {
    const AcumIna &a = acIna[1];
    r.vo = (float)(a.sv / a.n);
    r.io = (float)(a.si / a.n);
    r.pout = (float)(a.sp / a.n);
    r.vShunt2 = (float)(a.ssh / a.n);
  }
  if (r.pin > ETA_PIN_MIN_W) r.eta = 100.0f * r.pout / r.pin;  // NAN se faltar algo

  // ---- ADS1115 ----
  if (adsOk) {
    bool f0 = false, f1 = false;
    float l0 = NAN, l1 = NAN;
    calcular(acAds[0], r.voMed, r.voRms, f0, l0);
    calcular(acAds[1], r.ioMed, r.ioRms, f1, l1);
    r.porFase = f0 && f1;
    r.lacunaGraus = fmaxf(l0, l1);  // NAN se nenhum dos dois for por fase
    r.nA0 = (uint16_t)(acAds[0].n > 65535u ? 65535u : acAds[0].n);
    r.nA1 = (uint16_t)(acAds[1].n > 65535u ? 65535u : acAds[1].n);
    if (acAds[0].n > 0) {
      r.vA0 = (float)(acAds[0].sCru / acAds[0].n);
      r.vA0max = acAds[0].maxCru;
    }
    if (acAds[1].n > 0) r.vA1 = (float)(acAds[1].sCru / acAds[1].n);
    r.pdc = r.voMed * r.ioMed;
    r.pac = r.voRms * r.ioRms;
    if (r.pac > 1e-3f) r.etaRet = 100.0f * r.pdc / r.pac;
    r.ioRMed = r.voMed / cal.rCarga;
    r.ioRRms = r.voRms / cal.rCarga;
  }

  // ---- Zero do ACS712: usa uma janela INTEIRA comecada depois do pedido ----
  if (zeroEstado == 2) terminarZero(r);
  else if (zeroEstado == 1) zeroEstado = 2;

  res = r;
  memset(acIna, 0, sizeof(acIna));
  memset(acAds, 0, sizeof(acAds));

  // Se um INA226 resetou (queda de alimentacao, ruido), configura de novo.
  for (uint8_t k = 0; k < 2; k++) {
    if (!inaOk[k]) continue;
    uint16_t lido = 0;
    if (i2cLe16(END_INA[k], INA_REG_CONFIG, lido) && lido != INA_CONFIG) {
      i2cEscreve16(END_INA[k], INA_REG_CONFIG, INA_CONFIG);
      comTempo(aviso()).printf("%s estava com outra configuracao: configurado de novo.\n", NOME_INA[k]);
    }
  }
}

// ===================================================================
//                         Deteccao
// ===================================================================
void medicaoDetectar(bool imprimir) {
  for (uint8_t k = 0; k < 2; k++) {
    uint8_t r = inaConfigurar(k);
    inaOk[k] = (r == 0);
    inaFalhas[k] = 0;
    inaUlt[k].ok = inaOk[k];
    if (imprimir) {
      if (r == 0) con().printf("%s: OK\n", NOME_INA[k]);
      else if (r == 1) con().printf("%s: NAO encontrado\n", NOME_INA[k]);
      else if (r == 2) con().printf("%s: responde, mas NAO e um INA226 (ID errado)\n", NOME_INA[k]);
      else con().printf("%s: responde, mas a configuracao nao confere\n", NOME_INA[k]);
    }
  }
  adsOk = adsConfigurar();
  adsFalhas = 0;
  adsConvertendo = false;
  if (imprimir) con().printf("ADS1115 (0x48, retificadores): %s\n", adsOk ? "OK" : "AUSENTE");
}

// Procura de novo, sem alarde, os que estao faltando (ex.: fio solto e
// recolocado, ADS1115 que chegou depois).
static void redetectarAusentes() {
  for (uint8_t k = 0; k < 2; k++) {
    if (inaOk[k] || !i2cResponde(END_INA[k])) continue;
    if (inaConfigurar(k) == 0) {
      inaOk[k] = true;
      inaFalhas[k] = 0;
      comTempo(aviso()).printf("%s encontrado.\n", NOME_INA[k]);
    }
  }
  if (!adsOk && i2cResponde(END_ADS) && adsConfigurar()) {
    adsOk = true;
    adsFalhas = 0;
    adsConvertendo = false;
    comTempo(aviso()).println("ADS1115 (0x48) encontrado.");
  }
}

void medicaoIniciar() {
  for (uint8_t k = 0; k < 2; k++) {
    inaUlt[k].ok = false;
    inaUlt[k].v = NAN;
    inaUlt[k].i = NAN;
    inaUlt[k].vShunt = NAN;
    inaUlt[k].saturado = false;
  }
  memset(acIna, 0, sizeof(acIna));
  memset(acAds, 0, sizeof(acAds));
  resultadosVazios(res);
  res.id = 0;

  Wire.begin(PINO_SDA, PINO_SCL, I2C_FREQ_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  medicaoDetectar(true);
  uint32_t agora = millis();
  janelaInicioMs = agora;
  inaUltimaMs = agora;
  ultimaDeteccaoMs = agora;
}

void medicaoAtualizar() {
  uint32_t agora = millis();

  // 1) INA226 a cada 50 ms
  if (agora - inaUltimaMs >= INA_PERIODO_MS) {
    inaUltimaMs = agora;
    inaLer(0);
    inaLer(1);
    inaNova = true;
    inaContador++;
  }

  // 2) ADS1115 (um passo por volta do loop)
  adsPasso();

  // 3) Fim da janela de 1 s
  if (agora - janelaInicioMs >= JANELA_MS) {
    janelaInicioMs = agora;
    fecharJanela();
  }

  // 4) Procura dispositivos ausentes
  if (agora - ultimaDeteccaoMs >= REDETECTAR_MS) {
    ultimaDeteccaoMs = agora;
    redetectarAusentes();
  }
}

bool inaPresente(uint8_t k) {
  return k < 2 && inaOk[k];
}

bool adsPresente() {
  return adsOk;
}

const LeituraIna &medicaoIna(uint8_t k) {
  return inaUlt[k < 2 ? k : 1];
}

uint32_t medicaoInaContador() {
  return inaContador;
}

bool medicaoInaNova() {
  bool n = inaNova;
  inaNova = false;
  return n;
}

const Resultados &medicaoResultados() {
  return res;
}

void medicaoPedirZero() {
  zeroEstado = 1;
}

bool medicaoZeroEmAndamento() {
  return zeroEstado != 0;
}
