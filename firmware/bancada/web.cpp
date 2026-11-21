/*
  web.cpp - Painel web pelo Wi-Fi. Ver web.h.

  Enderecos:
    /                 pagina do painel
    /dados            medidas e estado atuais (JSON)
    /hist?desde=ID    historico de 1 s depois da janela ID (JSON, ate 600 pontos)
    /rapido?desde=N   leituras dos INA226 depois da leitura N (JSON, ate 600 pontos)
    /registro.csv     registro CSV (virgula decimal, para o Excel; ?ponto=1 = ponto decimal)
    /salvo.csv        copia salva na flash
    /reg/iniciar?ms=1000 | /reg/parar | /reg/apagar | /reg/salvar
    /ctl/pegar        pega o controle (um aparelho por vez); devolve o segredo t
    /ctl/hb?t=        sinal de vida (a pagina manda a cada 0,5 s); sem sinal por WEB_HOMEM_MORTO_MS -> desliga e solta
    /ctl/soltar?t=    desliga a saida e solta o controle (PAINEL_MANUAL 0: fica livre; 1: volta aos pots)
    /cmd?c=0          DESLIGA TUDO (qualquer aparelho, sem segredo)
    /cmd?c=...&t=     botoes da pagina: modo, l, d, a, f, rampa, p, r, zero, tela; so de quem tem o controle
    /cmd?caixa=1&c=...&t=   caixa "Comando (igual a serial)": qualquer comando da
                      serial, com as mesmas regras (o "l" pede o segundo "l"). Sem o controle, so os de
                      leitura (cmdSoLeitura). O "pot" e recusado (cmdForaDaCaixa); o "log" vale,
                      porque as linhas dele aparecem no monitor da pagina.
    /serial?desde=N   espelho do Monitor Serial, so leitura, qualquer aparelho:
                      o texto escrito na serial a partir do byte N (ate ESPELHO_RESP_MAX por vez), em
                      JSON: {"ini","prox","fim","mais","perdeu","reinicio","txt"}. A proxima consulta
                      usa desde=prox. Se o byte N ja foi sobrescrito, comeca no mais antigo, com a
                      linha "(... texto antigo descartado)"; se N passa do total, a placa reiniciou.
*/
#include <Arduino.h>
#include "config.h"
#include "web.h"
#include <WiFi.h>
#include <WebServer.h>
#include <FFat.h>
#include <math.h>
#include "registro.h"
#include "medicao.h"
#include "controle.h"
#include "pwm_conversores.h"
#include "disparo_scr.h"
#include "comandos.h"
#include "tela.h"

extern const char PAGINA_PAINEL[];  // fim deste arquivo

static WebServer servidor(WEB_PORTA);
static bool ativo = false;
static IPAddress ipAP;

// ---------------- retrato das medidas (escrito no loop, lido no servidor) ----------------
struct Retrato {
  uint32_t tms, id;
  uint8_t modo, tela;
  bool ligado, pelaSerial, scrSinc, scrPol, telaAuto;
  float dAtual, dAlvo, dMax, alfaAtual, alfaAlvo, fRede;
  uint32_t fPwm;
  bool ina0, ina1, ads;
  float vin, iin, pin, vo, io, pout, eta;
  float voMed, voRms, ioMed, ioRms, pdc, pac, etaRet;
  char protecao[48];     // so o aviso atual (controleProtecaoAtual), "" depois de religar ou pegar
  uint32_t protecaoHa;   // segundos desde esse desligamento
  // Os mesmos valores do "?"
  uint32_t pulsoUs, dispA, dispB;   // largura do pulso de gate; semiciclos disparados em A e em B
  bool zc;                          // chegam pulsos validos de ZC
  float semiUs, wUs;                // T/2 e W do pulso de ZC (filtrados)
  float rCarga, ioRMed;             // R da carga (cal r) e Io = Vo_med / R
  float teoMed, teoRms;             // Vo teoricas (SCR: com o alfa atual; RET_NC: meia onda)
  float teoMedP, teoRmsP;           // RET_NC: as da ponte
  float teoVo, teoVoAtraso;         // conversores: Vo teorica (Buck: tambem com o atraso do IRS2104)
};
static Retrato retrato;
static volatile uint32_t seqRetrato = 0;   // impar = sendo escrito
static uint32_t ultimoRetratoMs = 0;

static void escreverRetrato() {
  Retrato r;
  const Resultados &m = medicaoResultados();
  Modo md = modoAtual();
  ScrResumo sr = scrResumo();
  r.tms = millis(); r.id = m.id; r.modo = (uint8_t)md;
  r.ligado = saidaLigada(); r.pelaSerial = !controlePeloPot();
  r.tela = telaPagina(); r.telaAuto = telaEhAuto();
  r.dAtual = pwmDAtual(); r.dAlvo = pwmDAlvo(); r.dMax = pwmDMax(); r.fPwm = pwmFrequencia();
  r.alfaAtual = sr.armado ? sr.alfaAtual : 180.0f; r.alfaAlvo = sr.alfaAlvo;
  r.scrSinc = sr.sincronizado; r.scrPol = sr.polOk; r.fRede = sr.sincronizado ? sr.freqRede : 0.0f;
  r.ina0 = inaPresente(0); r.ina1 = inaPresente(1); r.ads = adsPresente();
  r.vin = m.vin; r.iin = m.iin; r.pin = m.pin; r.vo = m.vo; r.io = m.io; r.pout = m.pout; r.eta = m.eta;
  r.voMed = m.voMed; r.voRms = m.voRms; r.ioMed = m.ioMed; r.ioRms = m.ioRms; r.pdc = m.pdc; r.pac = m.pac; r.etaRet = m.etaRet;
  strncpy(r.protecao, controleProtecaoAtual(), sizeof(r.protecao) - 1); r.protecao[sizeof(r.protecao) - 1] = 0;
  r.protecaoHa = (r.tms - controleUltimaProtecaoMs()) / 1000;
  r.pulsoUs = sr.pulso_us; r.dispA = sr.dispA; r.dispB = sr.dispB;
  r.zc = sr.zcPresente; r.semiUs = sr.semi_us; r.wUs = sr.w_us;
  r.rCarga = cal.rCarga; r.ioRMed = m.ioRMed;
  // Teoricos com as mesmas contas e o mesmo Vm do "?"
  r.teoMed = r.teoRms = r.teoMedP = r.teoRmsP = r.teoVo = r.teoVoAtraso = NAN;
  if (modoScr(md)) {             // alfa atual; com o disparo desligado, 0 V (como no "?")
    r.teoMed = sr.armado ? scrVoMedTeorica(sr.modo, r.alfaAtual) : 0.0f;
    r.teoRms = scrVoRmsTeorica(sr.modo, r.alfaAtual);
  } else if (md == MODO_RET_NC) {   // diodos: alfa = 0, meia onda e ponte
    r.teoMed = scrVoMedTeorica(1, 0.0f); r.teoRms = scrVoRmsTeorica(1, 0.0f);
    r.teoMedP = scrVoMedTeorica(2, 0.0f); r.teoRmsP = scrVoRmsTeorica(2, 0.0f);
  } else {
    float dq1;
    teoVoConversor(md == MODO_BOOST, m.vin, r.dAtual, r.teoVo, r.teoVoAtraso, dq1);
  }
  seqRetrato = seqRetrato + 1;   // impar: escrevendo
  __sync_synchronize();
  retrato = r;
  __sync_synchronize();
  seqRetrato = seqRetrato + 1;   // par: pronto
}

static Retrato lerRetrato() {
  Retrato r;
  for (int t = 0; t < 50; t++) {
    uint32_t s1 = seqRetrato;
    if (s1 & 1) { delay(1); continue; }
    __sync_synchronize();
    r = retrato;
    __sync_synchronize();
    if (seqRetrato == s1) return r;
  }
  return r;
}

// ---------------- controle pelo painel (um aparelho por vez, com homem-morto) ----------------
// Escrito pelo servidor (nucleo 0) e lido pelo loop (nucleo 1) sem trava que desligue
// interrupcoes. Na corrida rara entre um sinal de vida e o vencimento, o controle cai
// e a saida desliga: o lado seguro.
static volatile bool     ctlAtivo = false;   // algum aparelho esta no controle
static volatile uint32_t ctlToken = 0;       // segredo do aparelho que esta no controle
static volatile uint32_t ctlNum   = 0;       // numero publico da posse (a pagina sabe se e a dela)
static volatile uint32_t ctlHbMs  = 0;       // ultimo sinal de vida
static char ctlMotivo[64] = "";             // por que o controle caiu (homem-morto)
static volatile bool     desligaPedido = false;  // DESLIGA TUDO: o loop executa na proxima volta
static volatile bool     apagarProtPedido = false;  // pegou o controle -> o loop apaga o aviso da protecao

// Comando para o loop: o servidor deixa o texto aqui e espera o loop executar.
static volatile bool     pedidoPendente = false;
static volatile uint32_t pedidoSeq = 0, respSeq = 0;
static char   pedidoTxt[LINHA_MAX + 4];      // o mesmo tamanho maximo de um comando da serial
static volatile bool     pedidoCaixa = false; // veio da caixa de comandos (e nao de um botao)?
static String pedidoResp;
static SemaphoreHandle_t pedidoFeito = nullptr;

static bool ctlVivo() { return ctlAtivo && (millis() - ctlHbMs) <= WEB_HOMEM_MORTO_MS; }

// ---------------- auxiliares de JSON ----------------
static void num(String &s, float v, int dec) {
  if (!isfinite(v)) { s += "null"; return; }
  char b[24]; snprintf(b, sizeof b, "%.*f", dec, v); s += b;
}
static void campoJ(String &s, const char *nome, float v, int dec) { s += '"'; s += nome; s += "\":"; num(s, v, dec); s += ','; }
static void campoB(String &s, const char *nome, bool v) { s += '"'; s += nome; s += "\":"; s += v ? "true" : "false"; s += ','; }
static void campoU(String &s, const char *nome, uint32_t v) { s += '"'; s += nome; s += "\":"; s += String((unsigned long)v); s += ','; }
static void campoT(String &s, const char *nome, const char *v) {
  s += '"'; s += nome; s += "\":\"";
  for (const char *p = v; *p; p++) { if (*p == '"' || *p == '\\') s += '\\'; s += *p; }
  s += "\",";
}

static void enviarCabecalhosSemCache() {
  servidor.sendHeader("Cache-Control", "no-store");
}

// ---------------- paginas ----------------
static void hPagina() {
  servidor.sendHeader("Cache-Control", "no-cache");
  servidor.send_P(200, "text/html; charset=utf-8", PAGINA_PAINEL);
}

static void hDados() {
  Retrato r = lerRetrato();
  String s; s.reserve(1500);
  s += '{';
  campoT(s, "placa", PLACA_NOME);
  campoJ(s, "t", r.tms / 1000.0f, 1);
  campoJ(s, "id", (float)r.id, 0);
  campoT(s, "modo", nomeModo((Modo)r.modo));
  campoB(s, "conversor", modoConversor((Modo)r.modo));
  campoB(s, "scr", modoScr((Modo)r.modo));
  campoB(s, "ligado", r.ligado);
  campoB(s, "serial", r.pelaSerial);
  campoB(s, "manual", PAINEL_MANUAL != 0);   // a bancada tem pots e botoes?
  campoJ(s, "tela", (float)r.tela, 0); campoB(s, "telaAuto", r.telaAuto);
  campoJ(s, "d", r.dAtual, 3); campoJ(s, "dAlvo", r.dAlvo, 3); campoJ(s, "dMax", r.dMax, 2); campoJ(s, "fPwm", (float)r.fPwm, 0);
  campoJ(s, "alfa", r.alfaAtual, 1); campoJ(s, "alfaAlvo", r.alfaAlvo, 1);
  campoB(s, "sinc", r.scrSinc); campoB(s, "pol", r.scrPol); campoJ(s, "fRede", r.fRede, 2);
  campoB(s, "ina0", r.ina0); campoB(s, "ina1", r.ina1); campoB(s, "ads", r.ads);
  campoJ(s, "vin", r.vin, 3); campoJ(s, "iin", r.iin, 4); campoJ(s, "pin", r.pin, 3);
  campoJ(s, "vo", r.vo, 3); campoJ(s, "io", r.io, 4); campoJ(s, "pout", r.pout, 3); campoJ(s, "eta", r.eta, 2);
  campoJ(s, "voMed", r.voMed, 3); campoJ(s, "voRms", r.voRms, 3); campoJ(s, "ioMed", r.ioMed, 4); campoJ(s, "ioRms", r.ioRms, 4);
  campoJ(s, "pdc", r.pdc, 3); campoJ(s, "pac", r.pac, 3); campoJ(s, "etaRet", r.etaRet, 2);
  campoT(s, "protecao", r.protecao); campoJ(s, "protecaoHa", (float)r.protecaoHa, 0);
  // pulso de gate, sincronismo, carga e teoricos (os mesmos do "?")
  campoU(s, "pulso", r.pulsoUs); campoB(s, "zc", r.zc); campoJ(s, "semi", r.semiUs, 0); campoJ(s, "wZc", r.wUs, 0);
  campoU(s, "dispA", r.dispA); campoU(s, "dispB", r.dispB);
  campoJ(s, "rCarga", r.rCarga, 2); campoJ(s, "ioRMed", r.ioRMed, 4); campoJ(s, "vm", VM_PICO, 2);
  campoJ(s, "teoMed", r.teoMed, 2); campoJ(s, "teoRms", r.teoRms, 2); campoJ(s, "teoMedP", r.teoMedP, 2); campoJ(s, "teoRmsP", r.teoRmsP, 2);
  campoJ(s, "teoVo", r.teoVo, 2); campoJ(s, "teoVoAtraso", r.teoVoAtraso, 2);
  // registro
  campoB(s, "gravando", registroGravando()); campoB(s, "cheio", registroCheio()); campoB(s, "salvando", registroSalvando());
  campoJ(s, "regMs", (float)registroPeriodoMs(), 0); campoJ(s, "regLinhas", (float)registroLinhas(), 0);
  campoJ(s, "regBytes", (float)registroBytes(), 0); campoJ(s, "regCap", (float)registroCapacidade(), 0);
  campoB(s, "temSalvo", registroTemSalvo()); campoJ(s, "salvoBytes", (float)registroTamanhoSalvo(), 0);
  campoT(s, "regMsg", registroMensagem());
  campoB(s, "psram", registroPsramOk());
  campoJ(s, "clientes", (float)WiFi.softAPgetStationNum(), 0);
  campoB(s, "ctl", ctlVivo()); campoJ(s, "ctlN", (float)ctlNum, 0); campoT(s, "ctlMotivo", ctlMotivo);
  campoJ(s, "homemMorto", (float)WEB_HOMEM_MORTO_MS, 0);
  s.setCharAt(s.length() - 1, '}');
  enviarCabecalhosSemCache();
  servidor.send(200, "application/json", s);
}

static PontoHist   bufHist[100];
static PontoRapido bufRap[150];

static void hHist() {
  uint32_t desde = servidor.hasArg("desde") ? (uint32_t)servidor.arg("desde").toInt() : 0;
  enviarCabecalhosSemCache();
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.send(200, "application/json", "");
  String s; s.reserve(16000);
  s = "{\"ult\":"; s += String((unsigned long)histUltimoId()); s += ",\"p\":[";
  bool primeiro = true; uint32_t enviados = 0;
  while (enviados < 600) {
    uint32_t n = histCopiar(desde, bufHist, 100);
    if (n == 0) break;
    for (uint32_t k = 0; k < n; k++) {
      const PontoHist &p = bufHist[k];
      if (!primeiro) s += ','; primeiro = false;
      s += '['; s += String((unsigned long)p.id); s += ','; num(s, p.tms / 1000.0f, 1); s += ','; s += String(p.modo); s += ','; s += String(p.ligado); s += ',';
      num(s, p.d, 3); s += ','; num(s, p.alfa, 1); s += ',';
      num(s, p.vin, 3); s += ','; num(s, p.iin, 4); s += ','; num(s, p.vo, 3); s += ','; num(s, p.io, 4); s += ',';
      num(s, p.pin, 3); s += ','; num(s, p.pout, 3); s += ','; num(s, p.eta, 2); s += ',';
      num(s, p.voMed, 3); s += ','; num(s, p.voRms, 3); s += ','; num(s, p.ioMed, 4); s += ','; num(s, p.ioRms, 4); s += ',';
      num(s, p.etaRet, 2); s += ','; num(s, p.fRede, 2); s += ']';
      desde = p.id;
    }
    enviados += n;
    servidor.sendContent(s); s = "";
    if (n < 100) break;
  }
  s += "]}";
  servidor.sendContent(s);
  servidor.sendContent("");
}

static void hRapido() {
  uint32_t desde = servidor.hasArg("desde") ? (uint32_t)servidor.arg("desde").toInt() : 0;
  enviarCabecalhosSemCache();
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.send(200, "application/json", "");
  String s; s.reserve(9000);
  s = "{\"ult\":"; s += String((unsigned long)rapidoUltimoN()); s += ",\"p\":[";
  bool primeiro = true; uint32_t enviados = 0;
  while (enviados < 600) {
    uint32_t n = rapidoCopiar(desde, bufRap, 150);
    if (n == 0) break;
    for (uint32_t k = 0; k < n; k++) {
      const PontoRapido &q = bufRap[k];
      if (!primeiro) s += ','; primeiro = false;
      s += '['; s += String((unsigned long)q.n); s += ','; num(s, q.tms / 1000.0f, 2); s += ',';
      num(s, q.vin, 3); s += ','; num(s, q.iin, 4); s += ','; num(s, q.vo, 3); s += ','; num(s, q.io, 4); s += ']';
      desde = q.n;
    }
    enviados += n;
    servidor.sendContent(s); s = "";
    if (n < 150) break;
  }
  s += "]}";
  servidor.sendContent(s);
  servidor.sendContent("");
}

// Envia texto CSV trocando o ponto decimal por virgula (Excel em portugues).
static char bufTxt[2049];
static void enviarCsvConvertido(bool virgula) {
  // Converte so dentro dos numeros: no CSV da bancada, '.' so aparece em numeros.
  if (!virgula) return;
  for (char *p = bufTxt; *p; p++) if (*p == '.') *p = ',';
}

static void hRegistroCsv() {
  bool virgula = !servidor.hasArg("ponto");
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"bancada_registro.csv\"");
  enviarCabecalhosSemCache();
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.send(200, "text/csv; charset=utf-8", "");
  servidor.sendContent("\xEF\xBB\xBF");  // BOM: o Excel reconhece os acentos
  size_t pos = 0;
  for (;;) {
    size_t n = registroLerTexto(pos, bufTxt, sizeof(bufTxt) - 1);
    if (n == 0) break;
    bufTxt[n] = 0; pos += n;
    enviarCsvConvertido(virgula);
    servidor.sendContent(bufTxt, n);
  }
  servidor.sendContent("");
}

static void hSalvoCsv() {
  bool virgula = !servidor.hasArg("ponto");
  if (!registroTemSalvo() || registroSalvando()) { servidor.send(404, "text/plain; charset=utf-8", "Nenhum registro salvo na flash (ou salvamento em andamento)."); return; }
  File f = FFat.open(REG_ARQ_SALVO, "r");
  if (!f) { servidor.send(404, "text/plain; charset=utf-8", "Arquivo nao encontrado."); return; }
  servidor.sendHeader("Content-Disposition", "attachment; filename=\"bancada_registro_salvo.csv\"");
  enviarCabecalhosSemCache();
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.send(200, "text/csv; charset=utf-8", "");
  servidor.sendContent("\xEF\xBB\xBF");
  for (;;) {
    size_t n = f.read((uint8_t *)bufTxt, sizeof(bufTxt) - 1);
    if (n == 0) break;
    bufTxt[n] = 0;
    enviarCsvConvertido(virgula);
    servidor.sendContent(bufTxt, n);
  }
  f.close();
  servidor.sendContent("");
}

static void responderOk(const char *msg) {
  String s = "{\"ok\":true,\"msg\":\""; s += msg; s += "\"}";
  enviarCabecalhosSemCache();
  servidor.send(200, "application/json", s);
}
static void responderErro(const char *msg) {
  String s = "{\"ok\":false,\"msg\":\""; s += msg; s += "\"}";
  enviarCabecalhosSemCache();
  servidor.send(200, "application/json", s);
}

static void hRegIniciar() {
  uint32_t ms = servidor.hasArg("ms") ? (uint32_t)servidor.arg("ms").toInt() : REG_PERIODO_PADRAO_MS;
  registroPedirInicio(ms);
  responderOk("pedido enviado");
}
static void hRegParar()  { registroPedirParada(); responderOk("pedido enviado"); }
static void hRegApagar() { registroPedirApagar(); responderOk("pedido enviado"); }
static void hRegSalvar() {
  if (registroPedirSalvar()) responderOk("salvando na flash");
  else responderErro("desligue a saida antes de salvar");
}

static void jsonTexto(String &s, const String &v) {
  s += '"';
  for (size_t i = 0; i < v.length(); i++) {
    char ch = v[i];
    if (ch == '"' || ch == '\\') { s += '\\'; s += ch; }
    else if (ch == '\n') s += "\\n";
    else if ((uint8_t)ch >= 0x20) s += ch;   // \r e outros controles: descarta
  }
  s += '"';
}
static void responderJ(bool ok, const String &msg, const String &extra = "") {
  String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"msg\":"; jsonTexto(s, msg); s += extra; s += '}';
  enviarCabecalhosSemCache();
  servidor.send(200, "application/json", s);
}
static uint32_t argU32(const char *nome) {
  return servidor.hasArg(nome) ? (uint32_t)strtoul(servidor.arg(nome).c_str(), nullptr, 10) : 0;
}

// ---------------- espelho do Monitor Serial ----------------
// So leitura, qualquer aparelho, sem o controle. Roda na tarefa do servidor (nucleo 0):
// copia um trecho do espelho (comandos.cpp, com a trava curta) e responde em JSON.
static char    *bufSerial    = nullptr;   // trecho copiado do espelho (na PSRAM)
static size_t   bufSerialCap = 0;
static uint32_t idBoot       = 0;         // muda a cada reset: a pagina percebe que a placa reiniciou
static void hSerial() {
  uint32_t desde = argU32("desde");
  uint32_t ini = 0, total = 0;
  bool perdeu = false, reinicio = false;
  size_t n = (bufSerial != nullptr) ? espelhoLer(desde, bufSerial, bufSerialCap, ini, total, perdeu, reinicio) : 0;
  size_t k = 0;
  if (perdeu) {   // o comeco foi sobrescrito: comeca na proxima linha inteira
    for (size_t j = 0; j < n && j < 300; j++) {
      if (bufSerial[j] == '\n') { k = j + 1; break; }
    }
  }
  uint32_t prox = ini + (uint32_t)n;
  String s;
  s.reserve(n + n / 8 + 200);
  s = "{\"boot\":"; s += String((unsigned long)idBoot);
  s += ",\"ini\":"; s += String((unsigned long)(ini + k));
  s += ",\"prox\":"; s += String((unsigned long)prox);
  s += ",\"fim\":"; s += String((unsigned long)total);
  s += ",\"mais\":"; s += (prox < total) ? "true" : "false";
  s += ",\"perdeu\":"; s += perdeu ? "true" : "false";
  s += ",\"reinicio\":"; s += reinicio ? "true" : "false";
  s += ",\"txt\":\"";
  if (reinicio) s += "(... a bancada reiniciou)\\n";
  if (perdeu) s += "(... texto antigo descartado)\\n";
  for (size_t j = k; j < n; j++) {
    char ch = bufSerial[j];
    if (ch == '"' || ch == '\\') { s += '\\'; s += ch; }
    else if (ch == '\n') s += "\\n";
    else if ((uint8_t)ch >= 0x80) s += '?';    // so ASCII (um trecho pode cortar um caractere ao meio)
    else if ((uint8_t)ch >= 0x20) s += ch;     // \r e outros controles: descarta
  }
  s += "\"}";
  enviarCabecalhosSemCache();
  servidor.send(200, "application/json", s);
}

// Executa um comando no loop (nucleo 1) e espera a resposta por ate 600 ms.
// (O loop responde em poucos ms; esperar mais seguraria o servidor e o sinal de vida.)
static String executarNoLoop(const char *c, bool caixa = false) {
  if (pedidoPendente) return "A bancada ainda esta executando o comando anterior. Tente de novo.";
  xSemaphoreTake(pedidoFeito, 0);   // limpa um "feito" atrasado
  uint32_t seq = pedidoSeq + 1;
  strncpy(pedidoTxt, c, sizeof(pedidoTxt) - 1); pedidoTxt[sizeof(pedidoTxt) - 1] = 0;
  pedidoCaixa = caixa;
  pedidoSeq = seq;
  __sync_synchronize();
  pedidoPendente = true;
  if (xSemaphoreTake(pedidoFeito, pdMS_TO_TICKS(600)) == pdTRUE && respSeq == seq) return pedidoResp;
  return "Comando enviado (a resposta nao chegou a tempo; veja a serial ou o estado).";
}

static void hCtlPegar() {
  if (ctlVivo()) { responderJ(false, "Outro aparelho esta no controle. Ele precisa soltar (ou parar de responder por 2 s)."); return; }
  ctlAtivo = false;
#if PAINEL_MANUAL
  String r = executarNoLoop("pot 0");   // a pagina comanda como a serial (d, a, l...)
#else
  String r = "Ligar sempre parte de D = 0 ou alfa = 180 graus.";   // sem pots: ja e "pela serial"
#endif
  uint32_t t = esp_random() | 1u;
  ctlToken = t; ctlHbMs = millis(); ctlNum = ctlNum + 1; ctlMotivo[0] = 0;
  apagarProtPedido = true;   // o aviso de um desligamento antigo sai da pagina e da tela (o "?" guarda)
  __sync_synchronize();
  ctlAtivo = true;
  char mt[24];
  saidaSerial().printf("%s[painel web] controle com o aparelho n. %lu\n", marcaTempo(mt, sizeof(mt), millis()), (unsigned long)ctlNum);
  String extra = ",\"t\":"; extra += String((unsigned long)t); extra += ",\"n\":"; extra += String((unsigned long)ctlNum);
  responderJ(true, String("Voce esta no controle. ") + r, extra);
}
static void hCtlHb() {
  uint32_t t = argU32("t");
  if (!ctlVivo() || t != ctlToken) { responderJ(false, ctlMotivo[0] ? String(ctlMotivo) : String("sem controle")); return; }
  ctlHbMs = millis();
  responderJ(true, "ok");
}
static void hCtlSoltar() {
  uint32_t t = argU32("t");
  if (!ctlVivo() || t != ctlToken) { responderJ(false, "Este aparelho nao esta no controle."); return; }
  ctlAtivo = false;
  String r = executarNoLoop("0");
  char mt[24];
  strncpy(ctlMotivo, "controle solto pelo aparelho", sizeof(ctlMotivo) - 1);
#if PAINEL_MANUAL
  r += executarNoLoop("pot 1");
  saidaSerial().printf("%s[painel web] controle solto: saida desligada, de volta aos potenciometros\n", marcaTempo(mt, sizeof(mt), millis()));
  responderJ(true, String("Controle devolvido aos potenciometros (saida desligada). ") + r);
#else
  saidaSerial().printf("%s[painel web] controle solto: saida desligada, controle livre (pagina ou serial)\n", marcaTempo(mt, sizeof(mt), millis()));
  responderJ(true, String("Controle solto (saida desligada). Fica livre para outro aparelho ou para a serial. ") + r);
#endif
}
static String primeiraPalavra(const String &c) {
  int esp = c.indexOf(' ');
  return esp < 0 ? c : c.substring(0, esp);
}
// Botoes da pagina: so estes comandos.
static bool cmdPermitido(const String &c) {
  String w = primeiraPalavra(c);
  static const char *OK[] = { "l", "liga", "modo", "d", "a", "f", "rampa", "p", "r", "zero", "tela" };
  for (const char *k : OK) if (w == k) return true;
  return false;
}
// Caixa de comandos: estes so mostram (nao mudam nada nem mexem nos sensores) e
// valem sem o controle. O comando inteiro tem de ser igual: "lim" sim, "lim io 0.2" nao.
static const char TXT_SO_LEITURA[] = "?, lim, cal, wifi, registro e ajuda";
static bool cmdSoLeitura(const String &c) {
  static const char *LER[] = { "?", "lim", "cal", "wifi", "registro", "ajuda", "help" };
  for (const char *k : LER) if (c == k) return true;
  return false;
}
// Caixa de comandos: estes nao fazem sentido pela pagina. Devolve o motivo (nullptr = aceito).
// (O "log" vale: as linhas dele aparecem no monitor da pagina.)
static const char *cmdForaDaCaixa(const String &c) {
  String w = primeiraPalavra(c);
  if (w == "pot") return "O pot nao vale pela pagina: quem comanda pela pagina e o aparelho que pegou o controle. "
                         "Use \"Pegar o controle\" e \"Soltar o controle\"; ou mande pot pela serial.";
  return nullptr;
}
static void hCmd() {
  String c = servidor.arg("c");
  const bool caixa = servidor.hasArg("caixa");
  c.trim(); c.toLowerCase(); c.replace(',', '.');
  if (c == "0") {   // DESLIGA TUDO: de qualquer aparelho, sem segredo, sem esperar a fila
    desligaPedido = true;
    responderJ(true, "DESLIGA TUDO enviado.");
    return;
  }
  uint32_t t = argU32("t");
  const bool dono = ctlVivo() && t == ctlToken;
  if (!caixa) {   // botoes da pagina
    if (!dono) { responderJ(false, "Pegue o controle antes (botao \"Pegar o controle\")."); return; }
    if (c.length() == 0 || c.length() > 40 || !cmdPermitido(c)) { responderJ(false, "Comando nao permitido pelo painel."); return; }
  } else {        // caixa "Comando (igual a serial)"
    if (c.length() == 0) { responderJ(false, "Digite um comando (ajuda mostra a lista)."); return; }
    if (c.length() > LINHA_MAX) {
      responderJ(false, String("Comando longo demais: no maximo ") + LINHA_MAX + " caracteres, como na serial.");
      return;
    }
    for (size_t i = 0; i < c.length(); i++) {
      if ((uint8_t)c[i] < 0x20) { responderJ(false, "Comando com caractere de controle: digite de novo."); return; }
    }
    const char *fora = cmdForaDaCaixa(c);
    if (fora != nullptr) { responderJ(false, fora); return; }
    if (!dono && !cmdSoLeitura(c)) {
      responderJ(false, String("Pegue o controle antes (botao \"Pegar o controle\"). Sem o controle, so os comandos que "
                               "so mostram: ") + TXT_SO_LEITURA + ". O 0 (DESLIGA TUDO) vale sempre.");
      return;
    }
  }
  if (dono) ctlHbMs = millis();
  responderJ(true, executarNoLoop(c.c_str(), caixa));
}

static void tarefaServidor(void *) {
  for (;;) {
    servidor.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void webIniciar() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_SENHA, WIFI_CANAL, 0, WIFI_MAX_CLIENTES)) {
    saidaSerial().println("ERRO ao criar a rede Wi-Fi: painel web indisponivel.");
    return;
  }
  ipAP = WiFi.softAPIP();
  servidor.on("/", hPagina);
  servidor.on("/dados", hDados);
  servidor.on("/hist", hHist);
  servidor.on("/rapido", hRapido);
  servidor.on("/registro.csv", hRegistroCsv);
  servidor.on("/salvo.csv", hSalvoCsv);
  servidor.on("/reg/iniciar", hRegIniciar);
  servidor.on("/reg/parar", hRegParar);
  servidor.on("/reg/apagar", hRegApagar);
  servidor.on("/reg/salvar", hRegSalvar);
  servidor.on("/ctl/pegar", hCtlPegar);
  servidor.on("/ctl/hb", hCtlHb);
  servidor.on("/ctl/soltar", hCtlSoltar);
  servidor.on("/cmd", hCmd);
  servidor.on("/serial", hSerial);
  bufSerial = (char *)ps_malloc(ESPELHO_RESP_MAX);   // trecho do espelho por resposta
  bufSerialCap = ESPELHO_RESP_MAX;
  if (bufSerial == nullptr) { bufSerial = (char *)malloc(2048); bufSerialCap = bufSerial ? 2048 : 0; }
  idBoot = esp_random();
  servidor.onNotFound([]() { servidor.send(404, "text/plain; charset=utf-8", "Pagina nao encontrada. Abra http://192.168.4.1/"); });
  pedidoFeito = xSemaphoreCreateBinary();
  servidor.begin();
  escreverRetrato();
  xTaskCreatePinnedToCore(tarefaServidor, "web", 12288, nullptr, 1, nullptr, 0);  // nucleo 0, junto do Wi-Fi
  ativo = true;
  saidaSerial().printf("Painel web: rede \"%s\" (senha \"%s\"), abra http://%s\n", WIFI_SSID, WIFI_SENHA, ipAP.toString().c_str());
}

void webAtualizar() {
  if (!ativo) return;
  if (apagarProtPedido) { apagarProtPedido = false; controleApagarProtecaoAtual(); }
  if (desligaPedido) {   // DESLIGA TUDO do painel: primeiro de tudo
    desligaPedido = false;
    comTempo(saidaSerial()).println("[painel web] DESLIGA TUDO");
    comandosCancelarLiga();   // um "l" da serial esperando confirmacao tambem cai
    controleDesligar(nullptr);
  }
  if (pedidoPendente) {
    __sync_synchronize();
    uint32_t seq = pedidoSeq;
    String r = comandoWeb(pedidoTxt, pedidoCaixa);
    pedidoResp = r; respSeq = seq;
    __sync_synchronize();
    pedidoPendente = false;
    xSemaphoreGive(pedidoFeito);
  }
  if (ctlAtivo && (millis() - ctlHbMs) > WEB_HOMEM_MORTO_MS) {   // homem-morto
    // So DESLIGA e solta o controle: nada e religado aqui.
    ctlAtivo = false;
    comandosCancelarLiga();
    bool estava = saidaLigada();
    if (estava) controleDesligar("painel web sem sinal (homem-morto)");
#if PAINEL_MANUAL
    controleUsarPot(true);   // com os pots montados, o comando volta para eles
#endif
    snprintf(ctlMotivo, sizeof(ctlMotivo), "%s", estava ? "o aparelho parou de responder: saida desligada" : "o aparelho parou de responder");
    comTempo(saidaSerial()).printf("[painel web] controle perdido: %s%s\n", ctlMotivo,
                  PAINEL_MANUAL ? " (de volta aos potenciometros)" : " (controle livre: pagina ou serial)");
  }
  uint32_t agora = millis();
  if (agora - ultimoRetratoMs >= 200) { ultimoRetratoMs = agora; escreverRetrato(); }
}

bool webAtivo() { return ativo; }
bool webNoControle() { return ctlVivo(); }

void webMostrar() {
  if (!ativo) { con().println("Painel web desligado (erro ao criar a rede)."); return; }
  con().printf("Wi-Fi: rede \"%s\", senha \"%s\", painel em http://%s | celulares conectados: %d\n",
                WIFI_SSID, WIFI_SENHA, ipAP.toString().c_str(), WiFi.softAPgetStationNum());
}

// ============================ PAGINA ============================
// Tudo dentro da pagina (sem internet na rede da placa): HTML, estilo e graficos em canvas.
const char PAGINA_PAINEL[] PROGMEM = R"PAGINA(<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bancada TCC</title>
<style>
:root{--f:#f4f6f9;--c:#fff;--t:#1b2430;--m:#5b6778;--b:#d8dee8;--az:#0b5cad;--vd:#1d8a4e;--vm:#c0392b;--am:#b7791f}
*{box-sizing:border-box}body{margin:0;background:var(--f);color:var(--t);font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;font-size:15px}
header{background:var(--az);color:#fff;padding:10px 14px;display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap;position:sticky;top:0;z-index:5}
header b{font-size:17px}header span{font-size:12px;opacity:.85}
main{max-width:1100px;margin:0 auto;padding:10px}
.est{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px}
.est div{background:var(--c);border:1px solid var(--b);border-radius:8px;padding:6px 10px}
.est small{display:block;color:var(--m);font-size:11px}.est b{font-size:16px}
.lig{background:#e7f6ee!important;border-color:var(--vd)!important}.lig b{color:var(--vd)}
.prot{background:#fdecea!important;border-color:var(--vm)!important}.prot b{color:var(--vm)}
.av{background:#fff8e1!important;border-color:var(--am)!important}.av b{color:var(--am)}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:8px;margin-bottom:10px}
.card{background:var(--c);border:1px solid var(--b);border-radius:8px;padding:8px 10px}
.card small{color:var(--m);font-size:12px}.card b{display:block;font-size:24px;font-variant-numeric:tabular-nums}.card i{font-style:normal;font-size:13px;color:var(--m)}
.card .teo{display:block;font-size:12px;color:var(--m);margin-top:2px;font-variant-numeric:tabular-nums}
section{background:var(--c);border:1px solid var(--b);border-radius:8px;padding:10px;margin-bottom:10px}
.tit{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap;font-weight:600;margin-bottom:6px}
canvas{width:100%;height:260px;display:block}
.leg{font-size:12px;color:var(--m);display:flex;gap:12px;flex-wrap:wrap;margin-top:4px}.leg span:before{content:"";display:inline-block;width:14px;height:3px;margin-right:5px;vertical-align:middle;background:var(--cor)}
button,select,a.bt{font:inherit;font-size:14px;padding:7px 12px;border-radius:6px;border:1px solid var(--b);background:#fff;color:var(--t);text-decoration:none;cursor:pointer}
button.pri{background:var(--az);color:#fff;border-color:var(--az)}button.per{color:var(--vm);border-color:var(--vm)}
.linha{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
#regMsg{font-size:13px;color:var(--m)}
footer{font-size:12px;color:var(--m);text-align:center;padding:6px 10px 20px}
.off{opacity:.45}
button.desl{background:var(--vm);color:#fff;border:2px solid #fff;font-weight:700;font-size:15px;padding:8px 14px}
button:disabled,input:disabled,select:disabled{opacity:.45;cursor:not-allowed}
input[type=number]{font:inherit;font-size:14px;padding:6px;border:1px solid var(--b);border-radius:6px;width:90px}
input[type=range]{width:min(320px,60vw)}
.resp{white-space:pre-wrap;font-size:13px;background:#f1f4f8;border-radius:6px;padding:6px 8px;margin-top:6px;min-height:1.6em}
.resp.erro{background:#fdecea;color:var(--vm)}
.nota{font-size:12px;color:var(--m);margin-top:6px}
#ctlEst,#cmdEst{font-weight:400;font-size:13px;color:var(--m)}
input[type=text]{font:14px ui-monospace,Consolas,monospace;padding:7px 8px;border:1px solid var(--b);border-radius:6px;flex:1;min-width:160px}
.term{white-space:pre-wrap;word-break:break-word;font:12px/1.35 ui-monospace,Consolas,monospace;background:#1b2430;color:#e8edf3;border-radius:6px;padding:8px;margin-top:6px;max-height:340px;overflow:auto;min-height:2.8em}
.term b{color:#9cc8ff;font-weight:600}.term .er{color:#ffb4a8}
.term.mon{height:min(60vh,420px);max-height:none;margin-top:0}
#monEst{font-size:12px;color:var(--m)}
#conf{position:fixed;left:8px;right:8px;bottom:8px;z-index:6;max-width:720px;margin:0 auto;background:#fff8e1;border:2px solid var(--am);border-radius:8px;padding:10px 12px;box-shadow:0 4px 18px rgba(0,0,0,.25)}
#conf .linha{margin:8px 0 0}#confS{font-size:12px;color:var(--m)}
</style></head><body>
<header><b>Bancada de eletrônica de potência</b><button class="desl" id="bOff">DESLIGA TUDO</button><span id="placa">conectando…</span></header>
<div id="conf" role="alertdialog" aria-labelledby="confT" style="display:none"><b id="confT"></b><div class="linha"><button class="pri" id="confSim">Confirmar</button><button id="confNao">Cancelar</button><span id="confS"></span></div></div>
<main>
<div class="est" id="est"></div>
<div class="cards" id="cards"></div>
<section id="secC"><div class="tit">Controle pela página <span id="ctlEst"></span></div>
<div class="linha"><button class="pri" id="bPegar">Pegar o controle</button><button id="bSoltar">Soltar o controle</button></div>
<div id="ctlCorpo">
<div class="linha">Modo <select id="selModo"><option value="boost">Boost</option><option value="buck">Buck</option><option value="nc">Retificador não controlado</option><option value="meia">Meia onda controlada</option><option value="ponte">Ponte semicontrolada</option></select><button id="bModo">Trocar o modo</button></div>
<div class="linha"><button class="pri" id="bLiga">Ligar a saída</button><button class="per" id="bDesl">Desligar a saída</button></div>
<div class="linha" id="lnD">D <input type="range" id="rD" min="0" max="0.5" step="0.01" value="0"><input type="number" id="nD" min="0" max="0.9" step="0.01" value="0"><button id="bD">Enviar D</button></div>
<div class="linha" id="lnF">Frequência <input type="number" id="nF" min="20000" max="150000" step="1000" style="width:110px"> Hz <button id="bF">Aplicar</button></div>
<div class="linha" id="lnA">α <input type="range" id="rA" min="0" max="180" step="1" value="180"><input type="number" id="nA" min="0" max="180" step="1" value="180"> ° <button id="bA">Enviar α</button></div>
<div class="linha" id="lnR">Rampa de α <input type="number" id="nR" min="0" max="30" step="0.5" value="1"> grau/semiciclo <button id="bR">Aplicar</button></div>
<div class="linha" id="lnP">Pulso de gate <input type="number" id="nP" min="100" max="2000" step="10" value="500"> µs <button id="bP">Aplicar</button><span class="nota" id="pAt"></span></div>
<div class="linha" id="lnC">Carga R <input type="number" id="nC" min="0.1" max="100000" step="0.01" value="47"> Ω <button id="bC">Aplicar</button><span class="nota" id="cAt"></span></div>
<div class="linha" id="lnZ"><button id="bZero">Zero do ACS712</button><span class="nota">saída desligada e sem corrente na carga</span></div>
<div class="linha">Tela OLED <select id="selTela"><option value="auto">troca sozinha</option><option value="0">página 0 (principal)</option><option value="1">página 1 (complemento)</option><option value="2">página 2 (estado)</option></select><button id="bTela">Aplicar</button></div>
</div>
<div id="ctlMsg" class="resp"></div>
<div class="nota">Um aparelho por vez. Quem está no controle manda sinal de vida a cada 0,5 s: se a página fechar, o celular bloquear a tela ou o Wi-Fi cair por 2 s, a saída desliga e o controle <span id="nHm">fica livre para outro aparelho ou para a serial</span>. Nada religa sozinho. Ligar pede confirmação (numa faixa no pé da página, que cancela sozinha em 10 s) e sempre parte de D = 0 ou α = 180°. O botão vermelho DESLIGA TUDO funciona em qualquer aparelho, mesmo sem o controle.</div>
</section>
<section id="secT"><div class="tit">Monitor serial e comandos <span id="cmdEst"></span></div>
<div class="linha"><input type="text" id="tCmd" maxlength="60" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false" enterkeyhint="send" placeholder="comando, ex.: ?  lim  cal  i2c  ajuda"><button class="pri" id="bCmd">Enviar</button></div>
<div id="cmdMsg" class="resp"></div>
<div class="linha"><button id="bPausa">Pausar</button><button id="bLimpa">Limpar tela</button><span id="monEst"></span></div>
<div class="term mon" id="saidaCmd"></div>
<div class="nota">O monitor mostra tudo o que a bancada escreve na serial, com o mesmo texto: o início, as respostas, os avisos, as proteções e o log. Ele atualiza a cada 0,5 s, em qualquer aparelho, mesmo sem o controle. Os comandos aparecem com "[painel web]" (desta página) ou "[serial]" (digitados na serial) na frente. O monitor rola sozinho, a não ser que você role para cima; <b>Pausar</b> congela a tela e <b>Limpar tela</b> só apaga o que está na tela. Vale todo comando da serial (<b>ajuda</b> mostra a lista), com as mesmas regras: o <b>l</b> pede o segundo l em até 5 s e o <b>lim</b> só muda com a saída desligada. Sem o controle, só os que só mostram: ?, lim, cal, wifi, registro e ajuda. O <b>log</b> vale pela página, e as linhas dele aparecem no monitor; o <b>pot</b> não. O <b>0</b> desliga tudo, de qualquer aparelho. As setas ↑ e ↓ repetem os comandos anteriores.</div>
</section>
<section><div class="tit">Histórico (médias de 1 s)
<select id="jan"><option value="120">últimos 2 min</option><option value="600" selected>últimos 10 min</option><option value="3600">última 1 h</option></select></div>
<canvas id="g1"></canvas><div class="leg" id="l1"></div></section>
<section id="secR"><div class="tit">Leitura rápida dos INA226 (a cada ~50 ms, últimos 30 s)</div>
<canvas id="g2"></canvas><div class="leg" id="l2"></div></section>
<section><div class="tit">Registro em CSV</div>
<div id="regEst">—</div>
<div class="linha">Uma linha a cada <select id="per"><option value="200">0,2 s</option><option value="500">0,5 s</option><option value="1000" selected>1 s</option><option value="5000">5 s</option><option value="10000">10 s</option><option value="60000">1 min</option></select>
<button class="pri" id="bIni">Iniciar</button><button id="bPar">Parar</button></div>
<div class="linha"><a class="bt" href="/registro.csv">Baixar CSV</a><button id="bSal">Salvar na flash</button><a class="bt" id="aSal" href="/salvo.csv">Baixar o salvo</a><button class="per" id="bApa">Apagar</button></div>
<div id="regMsg"></div></section>
<footer>Sem o controle, a página só mostra (e a caixa de comandos aceita os que só mostram). Com o controle, ela manda os mesmos comandos da serial, com os mesmos limites e proteções. <span id="fBot">A serial da bancada continua valendo, como reserva.</span><br>Os valores são médias de 1 s, as mesmas da tela OLED. Os "teóricos" dos cartões são ideais (carga R, sem quedas), com as mesmas contas do ?: <span id="fTeo"></span>.</footer>
</main>
<script>
const $=i=>document.getElementById(i);
const br=(v,d)=>v==null||!isFinite(v)?'—':v.toFixed(d).replace('.',',');
let H=[],R=[],ultH=0,ultR=0,D=null;
function est(){
  if(!D)return;
  const s=[];
  s.push(`<div><small>modo</small><b>${D.modo}</b></div>`);
  s.push(`<div class="${D.ligado?'lig':''}"><small>saída</small><b>${D.ligado?'LIGADA':'desligada'}</b></div>`);
  if(D.conversor)s.push(`<div><small>D atual / alvo</small><b>${br(D.d,3)} / ${br(D.dAlvo,3)}</b></div><div><small>frequência</small><b>${br(D.fPwm/1000,1)} kHz</b></div>`);
  if(D.scr)s.push(`<div><small>α atual / alvo</small><b>${br(D.alfa,1)}° / ${br(D.alfaAlvo,1)}°</b></div>`);
  if(!D.conversor){   // sincronismo (os mesmos valores do ?)
    s.push(`<div class="${D.sinc?'':'av'}"><small>rede</small><b>${D.sinc?br(D.fRede,2)+' Hz':(D.zc?'sem sincronismo':'sem sinal de ZC')}</b></div>`);
    s.push(`<div title="Largura W do pulso de ZC e meio período T/2, medidos pela placa (os mesmos do ?)"><small>ZC: W / T/2</small><b>${D.zc?br(D.wZc,0)+' / '+br(D.semi,0)+' µs':'—'}</b></div>`);
    const pol=D.pol?'OK':(D.modo=='RET_PONTE'?'ausente: modo reserva':(D.modo=='RET_MEIA'?'ausente: não dispara':'ausente'));
    s.push(`<div class="${D.pol||!D.scr?'':'av'}" title="${D.modo=='RET_PONTE'?'Sem o POL, a ponte dispara o GATE_A e o GATE_B em todo semiciclo (modo reserva).':(D.modo=='RET_MEIA'?'Sem o POL, a meia onda não dispara: não sabe qual é o semiciclo positivo.':'O POL só é usado nos modos com disparo.')}"><small>sinal POL</small><b>${pol}</b></div>`);
    if(D.scr)s.push(`<div title="Semiciclos em que cada gate deu pulso; zeram ao ligar o disparo"><small>semiciclos disparados A / B</small><b>${D.dispA} / ${D.dispB}</b></div>`);
  }
  s.push(`<div><small>controle</small><b>${D.ctl?'pelo painel web':(D.serial?'pela serial':'pelos pots')}</b></div>`);
  if(D.protecao){const h=D.protecaoHa||0,ha=h<120?h+' s':Math.floor(h/60)+' min';s.push(`<div class="prot" title="Some quando a saída liga de novo ou um aparelho pega o controle. O ? da serial mostra sempre o último."><small>último desligamento automático, há ${ha}</small><b>${D.protecao}</b></div>`);}
  $('est').innerHTML=s.join('');
  // Teoricos: ideais, com as mesmas contas e o mesmo Vm do ? (calculados na placa)
  const tV=v=>br(v,2)+' V';
  let c,tMed='',tRms='',tVo='';
  if(D.conversor){if(D.teoVo!=null)tVo=D.modo=='BUCK'?`teórica D·Vin: ${tV(D.teoVo)}`+(D.teoVoAtraso!=null?` (${tV(D.teoVoAtraso)} com o atraso do IRS2104)`:''):`teórica Vin/(1−D): ${tV(D.teoVo)}`;}
  else if(D.scr){const a=D.alfa>=179?'sem disparo':'α = '+br(D.alfa,1)+'°';tMed=`teórico (${a}): ${tV(D.teoMed)}`;tRms=`teórico (${a}): ${tV(D.teoRms)}`;}
  else{tMed=`teórico: meia onda ${tV(D.teoMed)}, ponte ${tV(D.teoMedP)}`;tRms=`teórico: meia onda ${tV(D.teoRms)}, ponte ${tV(D.teoRmsP)}`;}
  if(D.conversor)c=[['Vin',D.vin,'V',2],['Iin',D.iin,'A',3],['Pin',D.pin,'W',2],['Vo',D.vo,'V',2,tVo],['Io',D.io,'A',3],['Pout',D.pout,'W',2],['η',D.eta,'%',1]];
  else c=[['Vo médio',D.voMed,'V',2,tMed],['Vo eficaz',D.voRms,'V',2,tRms],['Io médio (ACS712)',D.ioMed,'A',3],['Io pela carga (Vo/R)',D.ioRMed,'A',3,`Vo médio / R, R = ${br(D.rCarga,2)} Ω`],['Io eficaz (ACS712)',D.ioRms,'A',3],['Pdc',D.pdc,'W',2],['Pca',D.pac,'W',2],['η',D.etaRet,'%',1]];
  const sem=D.conversor?!(D.ina0||D.ina1):!D.ads;
  $('cards').innerHTML=c.map(x=>`<div class="card ${sem?'off':''}"><small>${x[0]}</small><b>${br(x[1],x[3])}<i> ${x[2]}</i></b>${x[4]?`<span class="teo">${x[4]}</span>`:''}</div>`).join('')+(sem?`<div class="card"><small>sensor</small><b style="font-size:14px">${D.conversor?'INA226 não responde':'ADS1115 não responde'}</b></div>`:'');
  $('placa').textContent=`${D.placa} · ${D.clientes} conectado(s) · ${br(D.t/60,1)} min ligado`;
  $('fTeo').textContent=`nos retificadores, Vm = ${br(D.vm,2)} V; nos conversores, a Vin medida e o D atual`;
  $('secR').style.display=D.conversor?'':'none';
  const kb=x=>br(x/1024,0)+' kB';
  $('regEst').innerHTML=(D.gravando?`<b style="color:#1d8a4e">● gravando</b> uma linha a cada ${br(D.regMs/1000,1)} s — `:`<b>parado</b> — `)+`${D.regLinhas} linhas, ${kb(D.regBytes)} de ${kb(D.regCap)}`+(D.cheio?' — <b style="color:#c0392b">cheio</b>':'')+(D.temSalvo?` · salvo na flash: ${kb(D.salvoBytes)}`:'')+(D.psram?'':' · <b>sem PSRAM</b>');
  $('aSal').style.display=D.temSalvo?'':'none';
  $('bSal').disabled=D.ligado||D.salvando;$('bApa').disabled=D.salvando;
  if(D.regMsg)$('regMsg').textContent=D.regMsg;
  ctlTela();
}
let tok=0,meuN=0,hbT=null,tPeg=0;
const tenho=()=>!!(tok&&D&&D.ctl&&D.ctlN==meuN);
function msgC(t,ok){const e=$('ctlMsg');e.textContent=t||'';e.className='resp'+(ok===false?' erro':'');}
function ctlTela(){
  if(tok&&Date.now()-tPeg>2500&&(!D.ctl||D.ctlN!=meuN))perdi(D.ctl?'outro aparelho pegou o controle':(D.ctlMotivo||'sem sinal da bancada'));
  const meu=tenho();
  $('ctlEst').textContent=D.ctl?(meu?'— este aparelho está no controle':'— outro aparelho está no controle'):(D.manual&&!D.serial?'— pelos potenciômetros':'— livre (a serial também comanda)');
  $('nHm').textContent=D.manual?'volta aos potenciômetros':'fica livre para outro aparelho ou para a serial';
  $('fBot').textContent=D.manual?'Os botões, os potenciômetros e a serial da bancada continuam valendo.':'A serial da bancada continua valendo, como reserva.';
  $('bPegar').disabled=D.ctl;$('bSoltar').disabled=!meu;
  $('cmdEst').textContent=meu?'— todos os comandos da serial':'— sem o controle: só ?, lim, cal, wifi, registro e ajuda';
  for(const el of $('ctlCorpo').querySelectorAll('button,input,select'))el.disabled=!meu;
  $('ctlCorpo').classList.toggle('off',!meu);
  $('lnD').style.display=D.conversor?'':'none';$('lnF').style.display=D.conversor?'':'none';
  $('lnA').style.display=D.scr?'':'none';$('lnR').style.display=D.scr?'':'none';$('lnZ').style.display=D.conversor?'none':'';
  $('lnP').style.display=D.scr?'':'none';$('lnC').style.display=D.conversor?'none':'';
  if(meu){$('bLiga').disabled=D.ligado||D.modo=='RET_NC';$('bDesl').disabled=!D.ligado;$('bD').disabled=!D.ligado;$('bA').disabled=!D.ligado;$('bZero').disabled=D.ligado;$('bC').disabled=D.ligado;$('nC').disabled=D.ligado;}
  $('rD').max=D.dMax;$('nD').max=D.dMax;
  const ativo=document.activeElement;
  if(ativo!==$('nF'))$('nF').value=D.fPwm;
  // Pulso de gate e carga R, com o valor atual (o campo so muda se nao estiver sendo editado)
  $('pAt').textContent='atual: '+D.pulso+' µs';
  $('cAt').textContent='atual: '+br(D.rCarga,2)+' Ω'+(D.ligado?' · desligue a saída para mudar':' · só com a saída desligada');
  if(ativo!==$('nP')&&!$('nP').dataset.mexeu)$('nP').value=D.pulso;
  if(ativo!==$('nC')&&!$('nC').dataset.mexeu&&D.rCarga!=null)$('nC').value=+D.rCarga.toFixed(2);
  if(ativo!==$('selModo')&&!$('selModo').dataset.mexeu)$('selModo').value={BOOST:'boost',BUCK:'buck',RET_NC:'nc',RET_MEIA:'meia',RET_PONTE:'ponte'}[D.modo]||'nc';
  if(ativo!==$('selTela')&&!$('selTela').dataset.mexeu)$('selTela').value=D.telaAuto?'auto':String(D.tela);
}
// Confirmacao dentro da pagina, e nao o confirm() do navegador: o confirm() trava o JS da pagina, o sinal
// de vida para e, com mais de 2 s na caixa, o homem-morto desligaria a saida e soltaria o controle.
let cfA=null;
function confFecha(m){if(!cfA)return;const a=cfA;cfA=null;clearInterval(a.t);$('conf').style.display='none';if(m)a.msg(m);}
function confFechaCtl(){if(cfA&&cfA.ctl)confFecha();}
function confirma(texto,sim,ctl){
  confFecha();
  const fim=Date.now()+10000;
  cfA={sim,ctl,msg:ctl?(m=>msgC(m,false)):(m=>{$('regMsg').textContent=m;}),t:0};
  const conta=()=>{const s=Math.ceil((fim-Date.now())/1000);if(s<=0)confFecha('Confirmação cancelada: 10 s sem resposta.');else $('confS').textContent='cancela sozinha em '+s+' s';};
  $('confT').textContent=texto;conta();cfA.t=setInterval(conta,250);$('conf').style.display='';
}
$('confSim').onclick=()=>{const a=cfA;confFecha();if(a)a.sim();};
$('confNao').onclick=()=>confFecha();
async function cmd(c,conf){
  if(conf)return confirma(conf,()=>cmd(c),true);
  try{const j=await pega('/cmd?c='+encodeURIComponent(c)+'&t='+tok);msgC(j.msg,j.ok);}catch(e){msgC('Falhou: sem conexão com a bancada.',false);}
  setTimeout(ciclo,300);
}
function paraHb(){if(hbT)clearInterval(hbT);hbT=null;}
function perdi(m){paraHb();tok=0;confFechaCtl();msgC('Este aparelho saiu do controle: '+(m||'sem sinal da bancada')+'.',false);if(D)ctlTela();}
function iniciaHb(){paraHb();hbT=setInterval(async()=>{if(!tok)return;try{const j=await pega('/ctl/hb?t='+tok);if(!j.ok)perdi(j.msg);}catch(e){}},500);}
async function pegar(){
  try{const j=await pega('/ctl/pegar');if(j.ok){tok=j.t;meuN=j.n;tPeg=Date.now();iniciaHb();}msgC(j.msg,j.ok);}catch(e){msgC('Falhou: sem conexão com a bancada.',false);}
  ciclo();
}
async function soltar(){
  if(!tok)return;const t=tok;paraHb();tok=0;confFechaCtl();
  try{const j=await pega('/ctl/soltar?t='+t);msgC(j.msg,j.ok);}catch(e){msgC('Falhou: sem conexão com a bancada.',false);}
  ciclo();
}
async function pega(u){const r=await fetch(u,{cache:'no-store'});return r.json();}
async function ciclo(){
  try{D=await pega('/dados');est();}catch(e){$('placa').textContent='sem conexão com a bancada…';}
}
async function cicloH(){
  try{const j=await pega('/hist?desde='+ultH);if(j.ult<ultH){H=[];ultH=0;}
    for(const p of j.p){H.push(p);ultH=p[0];}if(H.length>3600)H=H.slice(H.length-3600);desenha1();}catch(e){}
}
async function cicloR(){
  if(D&&!D.conversor)return;
  try{const j=await pega('/rapido?desde='+ultR);if(j.ult<ultR){R=[];ultR=0;}
    for(const p of j.p){R.push(p);ultR=p[0];}if(R.length>2400)R=R.slice(R.length-2400);desenha2();}catch(e){}
}
function grafico(cv,series,tMin,tMax,leg){
  const dpr=window.devicePixelRatio||1,W=cv.clientWidth,Hh=cv.clientHeight;cv.width=W*dpr;cv.height=Hh*dpr;
  const g=cv.getContext('2d');g.scale(dpr,dpr);g.clearRect(0,0,W,Hh);g.font='11px system-ui,Arial';
  const m={e:46,d:46,c:8,b:22},x0=m.e,x1=W-m.d,y0=m.c,y1=Hh-m.b;
  const faixa=eixo=>{let a=Infinity,b=-Infinity;for(const s of series)if(s.eixo==eixo)for(const p of s.p){if(p[0]<tMin||p[1]==null)continue;a=Math.min(a,p[1]);b=Math.max(b,p[1]);}
    if(!isFinite(a))return null;if(a>0&&a<0.4*b)a=0;if(b-a<1e-6){a-=0.5;b+=0.5;}const f=(b-a)*0.08;return[a-f,b+f];};
  const E=faixa('e'),Dd=faixa('d');
  g.strokeStyle='#e3e8ef';g.fillStyle='#5b6778';g.lineWidth=1;
  for(let k=0;k<=4;k++){const y=y0+(y1-y0)*k/4;g.beginPath();g.moveTo(x0,y);g.lineTo(x1,y);g.stroke();
    if(E){g.textAlign='right';g.fillText(br(E[1]-(E[1]-E[0])*k/4,E[1]-E[0]>20?0:(E[1]-E[0]>2?1:2)),x0-4,y+4);}
    if(Dd){g.textAlign='left';g.fillText(br(Dd[1]-(Dd[1]-Dd[0])*k/4,Dd[1]-Dd[0]>2?2:3),x1+4,y+4);}}
  g.textAlign='center';for(let k=0;k<=4;k++){const t=tMin+(tMax-tMin)*k/4,x=x0+(x1-x0)*k/4;g.fillText(((t-tMax)).toFixed(0)+' s',x,Hh-6);}
  g.textAlign='left';if(E)g.fillText(series.filter(s=>s.eixo=='e')[0].u,2,10);if(Dd){g.textAlign='right';g.fillText(series.filter(s=>s.eixo=='d')[0].u,W-2,10);}
  for(const s of series){const F=s.eixo=='e'?E:Dd;if(!F)continue;g.strokeStyle=s.cor;g.lineWidth=s.w||1.8;g.beginPath();let on=false;
    for(const p of s.p){if(p[0]<tMin||p[1]==null){on=false;continue;}const x=x0+(x1-x0)*(p[0]-tMin)/(tMax-tMin),y=y1-(y1-y0)*(p[1]-F[0])/(F[1]-F[0]);
      if(on)g.lineTo(x,y);else{g.moveTo(x,y);on=true;}}g.stroke();}
  leg.innerHTML=series.map(s=>`<span style="--cor:${s.cor}">${s.n} (${s.u})</span>`).join('');
}
function desenha1(){
  if(!H.length)return;const tMax=H[H.length-1][1],jan=+$('jan').value,tMin=tMax-jan;
  const col=(i)=>H.map(p=>[p[1],p[i]]);const conv=D?D.conversor:true;
  const s=conv?[{n:'Vo',u:'V',eixo:'e',cor:'#0b5cad',p:col(8)},{n:'Vin',u:'V',eixo:'e',cor:'#7aa7d9',p:col(6)},{n:'Io',u:'A',eixo:'d',cor:'#c0392b',p:col(9)},{n:'Iin',u:'A',eixo:'d',cor:'#e59a8f',p:col(7)}]
              :[{n:'Vo médio',u:'V',eixo:'e',cor:'#0b5cad',p:col(13)},{n:'Vo eficaz',u:'V',eixo:'e',cor:'#7aa7d9',p:col(14)},{n:'Io médio',u:'A',eixo:'d',cor:'#c0392b',p:col(15)}];
  grafico($('g1'),s,tMin,tMax,$('l1'));
}
function desenha2(){
  if(!R.length)return;const tMax=R[R.length-1][1],tMin=tMax-30;const col=i=>R.map(p=>[p[1],p[i]]);
  grafico($('g2'),[{n:'Vo',u:'V',eixo:'e',cor:'#0b5cad',p:col(4)},{n:'Vin',u:'V',eixo:'e',cor:'#7aa7d9',p:col(2)},{n:'Io',u:'A',eixo:'d',cor:'#c0392b',p:col(5)},{n:'Iin',u:'A',eixo:'d',cor:'#e59a8f',p:col(3)}],tMin,tMax,$('l2'));
}
async function acao(u,msg){try{const j=await pega(u);$('regMsg').textContent=j.msg||msg;}catch(e){$('regMsg').textContent='falhou: sem conexão';}setTimeout(ciclo,400);}
$('bIni').onclick=()=>acao('/reg/iniciar?ms='+$('per').value,'iniciando');
$('bPar').onclick=()=>acao('/reg/parar','parando');
$('bSal').onclick=()=>acao('/reg/salvar','salvando');
$('bApa').onclick=()=>confirma('Apagar o registro da memória? (o que foi salvo na flash continua)',()=>acao('/reg/apagar','apagando'),false);
$('bOff').onclick=async()=>{confFechaCtl();try{const j=await pega('/cmd?c=0');msgC(j.msg,j.ok);}catch(e){msgC('Falhou: sem conexão com a bancada. '+(D&&D.manual?'Use o botão LIGA-DESLIGA da bancada, ':'Mande 0 pela serial, ')+'aperte o RST da placa ou desligue a fonte!',false);}setTimeout(ciclo,300);};
$('bPegar').onclick=pegar;$('bSoltar').onclick=soltar;
$('selModo').onchange=()=>{$('selModo').dataset.mexeu=1;};
$('bModo').onclick=()=>{const m=$('selModo').value;delete $('selModo').dataset.mexeu;cmd('modo '+m,D&&D.ligado?'Trocar o modo desliga a saída. Continuar?':null);};
$('bLiga').onclick=()=>cmd('l',`Ligar a saída no modo ${D.modo}? Ela começa em ${D.conversor?'D = 0':'α = 180° (sem disparo)'} e só sobe quando você mandar.`);
$('bDesl').onclick=()=>{confFechaCtl();cmd('0');};
const parD=(a,b)=>{$(a).oninput=()=>{$(b).value=$(a).value;};};parD('rD','nD');parD('nD','rD');parD('rA','nA');parD('nA','rA');
$('bD').onclick=()=>cmd('d '+$('nD').value);
$('bA').onclick=()=>cmd('a '+$('nA').value);
$('bF').onclick=()=>cmd('f '+$('nF').value,D&&D.ligado?'Mudar a frequência desliga a saída (D volta a 0). Continuar?':null);
$('bR').onclick=()=>cmd('rampa '+$('nR').value);
$('nP').oninput=()=>{$('nP').dataset.mexeu=1;};$('nC').oninput=()=>{$('nC').dataset.mexeu=1;};
$('bP').onclick=()=>{delete $('nP').dataset.mexeu;cmd('p '+$('nP').value);};
$('bC').onclick=()=>{delete $('nC').dataset.mexeu;cmd('r '+$('nC').value);};
$('bZero').onclick=()=>cmd('zero');
$('selTela').onchange=()=>{$('selTela').dataset.mexeu=1;};
$('bTela').onclick=()=>{const v=$('selTela').value;delete $('selTela').dataset.mexeu;cmd('tela '+v);};
for(const [i,b] of [['nD','bD'],['nA','bA'],['nF','bF'],['nR','bR'],['nP','bP'],['nC','bC']])$(i).onkeydown=e=>{if(e.key==='Enter')$(b).click();};
const histC=[];let iHist=0;
// Monitor serial: /serial?desde=N devolve o texto da serial a partir do byte N.
const MON_MAX=2000;let sDesde=0,sBoot=null,sPausa=false,sOcupado=false,monTxt='',monLin=0;
function monPoe(t){
  if(!t)return;const e=$('saidaCmd');
  const grudado=e.scrollTop+e.clientHeight>=e.scrollHeight-8;   // rolou para cima? entao nao rola sozinho
  monTxt+=t;monLin+=(t.match(/\n/g)||[]).length;
  if(monLin>MON_MAX+200){const l=monTxt.split('\n');monTxt=l.slice(l.length-MON_MAX-1).join('\n');monLin=MON_MAX;e.textContent=monTxt;}
  else e.append(document.createTextNode(t));
  if(grudado)e.scrollTop=e.scrollHeight;
}
async function cicloS(){
  if(sPausa||sOcupado)return;sOcupado=true;
  try{
    for(let k=0;k<12;k++){   // ao abrir a pagina: o buffer inteiro, em pedacos de 8 kB
      const j=await pega('/serial?desde='+sDesde);
      if(sBoot!==null&&j.boot!==sBoot){sBoot=j.boot;sDesde=0;monPoe('\n(... a bancada reiniciou)\n');continue;}
      sBoot=j.boot;monPoe(j.txt);sDesde=j.prox;
      if(!j.mais)break;
    }
    $('monEst').textContent=sPausa?'pausado':'';
  }catch(e){$('monEst').textContent='monitor: sem conexão com a bancada…';}
  sOcupado=false;
}
function msgCmd(t,ok){const e=$('cmdMsg');e.textContent=t||'';e.className='resp'+(ok===false?' erro':'');}
async function enviaCmd(){
  const c=$('tCmd').value.trim();if(!c)return;
  if(histC[histC.length-1]!==c){histC.push(c);if(histC.length>30)histC.shift();}iHist=histC.length;$('tCmd').value='';
  try{const j=await pega('/cmd?caixa=1&c='+encodeURIComponent(c)+'&t='+tok);
    const l=(j.msg||'').split('\n')[0];msgCmd(j.ok?'> '+c+(l?': '+(l.length>140?l.slice(0,140)+'…':l):''):j.msg,j.ok);}
  catch(e){msgCmd('> '+c+': falhou, sem conexão com a bancada.',false);}
  cicloS();setTimeout(ciclo,300);
}
$('bCmd').onclick=enviaCmd;
$('bLimpa').onclick=()=>{monTxt='';monLin=0;$('saidaCmd').textContent='';};
$('bPausa').onclick=()=>{sPausa=!sPausa;$('bPausa').textContent=sPausa?'Continuar':'Pausar';$('monEst').textContent=sPausa?'pausado':'';if(!sPausa)cicloS();};
$('tCmd').onkeydown=e=>{
  if(e.key==='Enter'){e.preventDefault();enviaCmd();}
  else if(e.key==='ArrowUp'&&histC.length){e.preventDefault();iHist=Math.max(0,iHist-1);$('tCmd').value=histC[iHist];}
  else if(e.key==='ArrowDown'&&histC.length){e.preventDefault();iHist=Math.min(histC.length,iHist+1);$('tCmd').value=histC[iHist]||'';}
};
window.addEventListener('pagehide',()=>{if(tok)navigator.sendBeacon('/ctl/soltar?t='+tok);});
$('jan').onchange=desenha1;window.onresize=()=>{desenha1();desenha2();};
ciclo();cicloH();cicloR();cicloS();setInterval(ciclo,1000);setInterval(cicloH,2000);setInterval(cicloR,500);setInterval(cicloS,500);
</script></body></html>)PAGINA";
