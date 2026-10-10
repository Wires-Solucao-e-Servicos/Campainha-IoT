// Central da campainha do estacionamento (ESP-01 + módulo relé).
// Recebe os avisos dos botões por HTTP e liga o buzzer pelo tempo configurado.
//   GET  /tocar?origem=porta1-entrada  -> toca o buzzer (aberto, é o que as NodeMCU usam)
//   GET  /                             -> status e configuração (usuário "admin", senha SENHA_ADMIN)
//   POST /salvar-toque, /salvar-rede, /restaurar -> chamados pelos formulários da página
//
// Por padrão a central pega IP automático (DHCP). As NodeMCU a encontram mandando
// "CAMPAINHA?" em broadcast UDP; a central responde "CAMPAINHA!" e elas usam o IP de
// quem respondeu. Quando o HTTP não chega (porta em outra faixa de IP, máscaras diferentes),
// a porta avisa em broadcast com "TOCAR <id> <origem>" e a central confirma com "TOCADO <id>".
// Para as pessoas, a central se anuncia como http://NOME_NA_REDE.local (mDNS).
//
// O tempo do toque, o Wi-Fi e o modo de IP ficam guardados na flash (EEPROM).
//
// Quando o Wi-Fi é trocado pela página, a central pergunta "CAMPAINHA-PORTAS?" em broadcast,
// cada porta responde "PORTA N", e a central envia o Wi-Fi novo para cada uma em
// POST http://IP_DA_PORTA/salvar-wifi (usuário "admin", senha SENHA_ADMIN, que tem que ser
// igual nas portas). Depois mostra quais portas confirmaram e reinicia.
//
// Rede de socorro "Campainha-Central" (senha SENHA_ADMIN, página em http://192.168.4.1):
// fica ligada nos 3 primeiros minutos depois de ligar a central e sempre que ela
// ficar mais de 30 s sem conseguir entrar no Wi-Fi. Serve para corrigir a configuração
// se um Wi-Fi ou IP errado for salvo.
//
// A página também reinicia a central e atualiza o firmware (POST /reiniciar, /atualizar).

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <Updater.h>
#include "config.h"

#if defined(RELE_VIA_GPIO) == defined(RELE_VIA_SERIAL)
#error "No config.h, deixe ativo só um dos dois: RELE_VIA_GPIO ou RELE_VIA_SERIAL"
#endif

// A rede de socorro usa essa senha, e o Wi-Fi só aceita senhas de 8 a 63 caracteres.
static_assert(sizeof(SENHA_ADMIN) - 1 >= 8 && sizeof(SENHA_ADMIN) - 1 <= 63,
              "SENHA_ADMIN precisa ter de 8 a 63 caracteres");

// No modo serial a serial conversa com o relé, então não dá para mandar mensagens de log.
#ifdef RELE_VIA_GPIO
#define LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

#define NOME_AP "Campainha-Central"
#define ENDERECO_POR_NOME NOME_NA_REDE ".local"

// Marca gravada no firmware. A atualização pela página só aceita um arquivo com esta
// marca, para não gravar por engano o firmware de uma porta na central.
const char MARCA_FIRMWARE[] = "<CAMPAINHA-FW:CENTRAL>";

// Mensagens UDP: estes valores têm que ser iguais no projeto porta-esp8266.
const uint16_t PORTA_DESCOBERTA = 4210;
const char PERGUNTA_DESCOBERTA[] = "CAMPAINHA?";
const char RESPOSTA_DESCOBERTA[] = "CAMPAINHA!";
const char PERGUNTA_PORTAS[] = "CAMPAINHA-PORTAS?";
const char RESPOSTA_PORTA[] = "PORTA ";       // seguido do número da porta
const char PEDIDO_TOQUE[] = "TOCAR ";         // seguido de "<id> <origem>": aviso de porta em outra faixa de IP
const char CONFIRMACAO_TOQUE[] = "TOCADO ";   // seguido de "<id>"
// Broadcast geral: chega a todos no mesmo Wi-Fi, seja qual for a faixa de IP de cada um.
const IPAddress BROADCAST_GERAL(255, 255, 255, 255);

const unsigned long AP_DEPOIS_DE_LIGAR_MS = 180000;  // rede de socorro aberta nos 3 primeiros minutos
const unsigned long AP_SEM_WIFI_MS = 30000;          // e quando ficar este tempo sem Wi-Fi
const unsigned long REINICIAR_SEM_WIFI_MS = 600000;  // reinicia se ficar 10 min sem Wi-Fi (e ninguém configurando)
const unsigned long USO_PAGINA_MS = 180000;          // abriu a página há menos que isso = está configurando

struct Config {
  uint32_t assinatura;
  uint32_t idFabrica;  // identifica os valores de fábrica do config.h que estavam valendo
  char ssid[33];
  char senha[65];
  uint8_t dhcp;        // 1 = IP automático, 0 = usa o IP fixo abaixo
  uint32_t ip, gateway, mascara;
  uint32_t tempoToqueMs;
};

const uint32_t ASSINATURA = 0xCA3A0002;

Config cfg;
ESP8266WebServer servidor(80);
WiFiUDP udpDescoberta;

bool tocando = false;
unsigned long tocandoDesde = 0;

String ultimaOrigem;
IPAddress ultimoAvisoIp;
bool ultimoAvisoOutraFaixa = false;
unsigned long ultimoAvisoMs = 0;
unsigned long totalAvisos = 0;

bool estavaConectado = false;
unsigned long semWifiDesde = 0;
bool apLigado = false;
bool janelaInicialAcabou = false;
bool buscaPausada = false;
bool acessouPagina = false;
unsigned long ultimoAcessoPagina = 0;

bool reiniciarPedido = false;
unsigned long reiniciarPedidoEm = 0;

// ===== Configuração salva =====

// Muda sempre que algum valor de fábrica do config.h muda. Assim, gravar o firmware
// com valores novos no config.h faz esses valores valerem de novo.
uint32_t idDaFabrica() {
  const char* texto = WIFI_SSID "|" WIFI_SENHA "|" IP_CENTRAL "|" IP_GATEWAY "|" IP_MASCARA;
  uint32_t h = 2166136261u;
  for (const char* c = texto; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
  h = (h ^ TEMPO_TOQUE_MS) * 16777619u;
  return (h ^ (USAR_DHCP ? 1u : 0u)) * 16777619u;
}

bool tempoValido(uint32_t ms) {
  return ms >= 100 && ms <= 60000;
}

// Máscara válida tem todos os bits 1 seguidos e depois só 0 (ex: 255.255.255.0).
bool mascaraValida(uint32_t mascara) {
  uint32_t zeros = ~__builtin_bswap32(mascara);  // IPAddress guarda os bytes na ordem da rede
  return mascara != 0 && (zeros & (zeros + 1)) == 0;
}

// O IP tem que estar na mesma faixa do roteador, senão a central fica fora da rede.
bool redeValida(uint32_t ip, uint32_t gateway, uint32_t mascara) {
  return mascaraValida(mascara) && ip != gateway &&
         (ip & mascara) == (gateway & mascara) &&
         (ip & ~mascara) != 0 && (ip | mascara) != 0xFFFFFFFF;  // nem endereço da rede, nem broadcast
}

void carregarFabrica() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.assinatura = ASSINATURA;
  cfg.idFabrica = idDaFabrica();
  strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid));
  strlcpy(cfg.senha, WIFI_SENHA, sizeof(cfg.senha));
  cfg.dhcp = USAR_DHCP ? 1 : 0;
  IPAddress ip;
  ip.fromString(IP_CENTRAL);
  cfg.ip = ip;
  ip.fromString(IP_GATEWAY);
  cfg.gateway = ip;
  ip.fromString(IP_MASCARA);
  cfg.mascara = ip;
  cfg.tempoToqueMs = TEMPO_TOQUE_MS;
}

void carregarConfig() {
  EEPROM.get(0, cfg);
  cfg.ssid[sizeof(cfg.ssid) - 1] = 0;
  cfg.senha[sizeof(cfg.senha) - 1] = 0;
  bool valida = cfg.assinatura == ASSINATURA && cfg.idFabrica == idDaFabrica() && cfg.ssid[0] &&
                cfg.dhcp <= 1 && tempoValido(cfg.tempoToqueMs) &&
                redeValida(cfg.ip, cfg.gateway, cfg.mascara);
  if (!valida) {
    LOG("Usando os valores de fabrica do config.h\n");
    carregarFabrica();
  }
}

void salvarConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// Endereço para voltar à página depois de reiniciar.
String enderecoDaCentral() {
  return cfg.dhcp ? String(ENDERECO_POR_NOME) : IPAddress(cfg.ip).toString();
}

// ===== Relé e toque =====

void rele(bool ligar) {
#ifdef RELE_VIA_SERIAL
  static const uint8_t CMD_LIGAR[] = {0xA0, 0x01, 0x01, 0xA2};
  static const uint8_t CMD_DESLIGAR[] = {0xA0, 0x01, 0x00, 0xA1};
  Serial.write(ligar ? CMD_LIGAR : CMD_DESLIGAR, 4);
#else
  bool nivel = RELE_ATIVO_EM_LOW ? !ligar : ligar;
  digitalWrite(PINO_RELE, nivel ? HIGH : LOW);
#endif
}

// Aperto durante o toque é ignorado: não toca de novo nem estica o tempo.
void tocar() {
  if (tocando) return;
  rele(true);
  tocando = true;
  tocandoDesde = millis();
}

void atualizarToque() {
  if (tocando && millis() - tocandoDesde >= cfg.tempoToqueMs) {
    rele(false);
    tocando = false;
  }
}

// Mantém só letras, números e '-' para a origem poder ir direto para a página.
String limparOrigem(const String& texto) {
  String limpo;
  for (size_t i = 0; i < texto.length() && limpo.length() < 32; i++) {
    char c = texto[i];
    if (isalnum(c) || c == '-') limpo += c;
  }
  return limpo.length() ? limpo : String("sem-origem");
}

// Aviso de uma porta, chegando por HTTP (normal) ou por broadcast (porta em outra faixa de IP).
void registrarAviso(const String& origem, IPAddress ip, bool outraFaixa) {
  ultimaOrigem = limparOrigem(origem);
  ultimoAvisoIp = ip;
  ultimoAvisoOutraFaixa = outraFaixa;
  ultimoAvisoMs = millis();
  totalAvisos++;
  LOG("Aviso de %s (%s)%s%s\n", ultimaOrigem.c_str(), ip.toString().c_str(),
      outraFaixa ? " por broadcast, de outra faixa de IP" : "", tocando ? " (ignorado: ja esta tocando)" : "");
  tocar();
}

// ===== Wi-Fi e descoberta =====

void conectarNaRede() {
  if (cfg.dhcp) {
    WiFi.config(0U, 0U, 0U);  // tudo zero = pedir IP ao roteador (DHCP)
  } else {
    WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.mascara));
  }
  WiFi.begin(cfg.ssid, cfg.senha);
}

void gerenciarWifi() {
  unsigned long agora = millis();
  bool conectado = WiFi.status() == WL_CONNECTED;
  if (conectado && !estavaConectado) {
    LOG("Wi-Fi conectado, IP %s, sinal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  if (!conectado && estavaConectado) LOG("Wi-Fi caiu\n");
  estavaConectado = conectado;

  if (conectado) {
    semWifiDesde = 0;
  } else if (semWifiDesde == 0) {
    semWifiDesde = agora;
  }
  bool muitoTempoSemWifi = !conectado && agora - semWifiDesde > AP_SEM_WIFI_MS;
  if (agora > AP_DEPOIS_DE_LIGAR_MS) janelaInicialAcabou = true;

  // Alguém está configurando: tem aparelho na rede de socorro e a página foi aberta há
  // pouco. Só estar conectado não conta: um celular que lembrou a rede de socorro e
  // entrou sozinho nela não pode impedir a volta para o roteador.
  uint8_t clientesAP = apLigado ? WiFi.softAPgetStationNum() : 0;
  bool configurando = clientesAP > 0 && acessouPagina && millis() - ultimoAcessoPagina < USO_PAGINA_MS;

  // Rede de socorro: não desliga enquanto alguém estiver configurando.
  bool precisaAP = !janelaInicialAcabou || muitoTempoSemWifi || configurando;
  if (precisaAP && !apLigado) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(NOME_AP, SENHA_ADMIN);
    apLigado = true;
    LOG("Rede de socorro \"%s\" ligada (http://%s)\n", NOME_AP, WiFi.softAPIP().toString().c_str());
  } else if (!precisaAP && apLigado) {
    WiFi.softAPdisconnect(true);
    apLigado = false;
    LOG("Rede de socorro desligada\n");
  }

  // Enquanto procura o Wi-Fi, o ESP fica trocando de canal e a rede de socorro fica
  // instável. Então, enquanto alguém está configurando e o Wi-Fi não conecta, para de procurar.
  bool deveProcurar = conectado || !configurando;
  if (!deveProcurar && !buscaPausada) {
    WiFi.disconnect();
    buscaPausada = true;
  } else if (deveProcurar && buscaPausada) {
    conectarNaRede();
    buscaPausada = false;
  }

  if (!conectado && !configurando && agora - semWifiDesde > REINICIAR_SEM_WIFI_MS) {
    LOG("Muito tempo sem Wi-Fi, reiniciando...\n");
    ESP.restart();
  }
}

bool naMinhaFaixa(IPAddress ip) {
  uint32_t mascara = WiFi.subnetMask();
  return (uint32_t(ip) & mascara) == (uint32_t(WiFi.localIP()) & mascara);
}

// Responde a quem mandou o pacote UDP atual. Se ele estiver em outra faixa de IP, a resposta
// direta iria para o roteador e se perderia; em broadcast ela chega, porque o Wi-Fi é o mesmo.
void responderUdp(const String& texto) {
  IPAddress origem = udpDescoberta.remoteIP();
  udpDescoberta.beginPacket(naMinhaFaixa(origem) ? origem : BROADCAST_GERAL, udpDescoberta.remotePort());
  udpDescoberta.print(texto);
  udpDescoberta.endPacket();
}

// Lê o pacote UDP que acabou de chegar (depois de parsePacket) como texto.
void lerPacote(char* msg, size_t tamanho) {
  int n = udpDescoberta.read(msg, tamanho - 1);
  msg[n > 0 ? n : 0] = 0;
}

// Mensagens das portas: procura pela central e aviso de toque por broadcast.
void tratarPacoteUdp(const char* msg) {
  IPAddress origem = udpDescoberta.remoteIP();
  if (strcmp(msg, PERGUNTA_DESCOBERTA) == 0) {
    responderUdp(RESPOSTA_DESCOBERTA);
    LOG("Procura respondida para %s%s\n", origem.toString().c_str(),
        naMinhaFaixa(origem) ? "" : " (outra faixa de IP, resposta em broadcast)");
  } else if (strncmp(msg, PEDIDO_TOQUE, strlen(PEDIDO_TOQUE)) == 0) {
    unsigned long id;
    char origemAviso[40];
    if (sscanf(msg + strlen(PEDIDO_TOQUE), "%lu %39s", &id, origemAviso) != 2) return;
    registrarAviso(origemAviso, origem, !naMinhaFaixa(origem));
    responderUdp(CONFIRMACAO_TOQUE + String(id));
  }
}

void responderDescoberta() {
  if (udpDescoberta.parsePacket() <= 0) return;
  char msg[64];
  lerPacote(msg, sizeof(msg));
  tratarPacoteUdp(msg);
}

// ===== Envio do Wi-Fi novo para as portas =====

struct Porta {
  uint8_t numero;
  IPAddress ip;
};

// Pergunta na rede quais portas estão conectadas. Retorna quantas responderam.
uint8_t buscarPortas(Porta* portas, uint8_t maximo) {
  uint8_t qtd = 0;
  // Pacotes que já estavam esperando: uma procura ou um aviso de porta ainda são atendidos.
  while (udpDescoberta.parsePacket() > 0) {
    char msg[64];
    lerPacote(msg, sizeof(msg));
    tratarPacoteUdp(msg);
  }

  for (int tentativa = 0; tentativa < 3; tentativa++) {
    // Pelo broadcast da própria faixa (funciona até se o roteador não informar gateway) e
    // pelo geral (alcança portas que estejam em outra faixa de IP).
    for (IPAddress destino : {WiFi.broadcastIP(), BROADCAST_GERAL}) {
      udpDescoberta.beginPacket(destino, PORTA_DESCOBERTA);
      udpDescoberta.write(PERGUNTA_PORTAS);
      udpDescoberta.endPacket();
    }

    unsigned long inicio = millis();
    while (millis() - inicio < 400) {
      if (udpDescoberta.parsePacket() > 0) {
        char msg[64];
        lerPacote(msg, sizeof(msg));
        size_t tamPrefixo = strlen(RESPOSTA_PORTA);
        if (strncmp(msg, RESPOSTA_PORTA, tamPrefixo) == 0) {
          int numero = atoi(msg + tamPrefixo);
          IPAddress ip = udpDescoberta.remoteIP();
          bool repetida = false;
          for (uint8_t i = 0; i < qtd; i++) repetida |= portas[i].ip == ip;
          if (numero > 0 && !repetida && qtd < maximo) portas[qtd++] = {(uint8_t)numero, ip};
        } else {
          tratarPacoteUdp(msg);  // uma porta procurando a central ou avisando bem nesta hora
        }
      }
      delay(5);
    }
  }
  return qtd;
}

String codificarUrl(const char* texto) {
  String r;
  for (const char* c = texto; *c; c++) {
    if (isalnum((uint8_t)*c) || strchr("-_.~", *c)) {
      r += *c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)*c);
      r += hex;
    }
  }
  return r;
}

bool enviarWifiParaPorta(IPAddress ip, const char* ssid, const char* senha) {
  WiFiClient cliente;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(cliente, "http://" + ip.toString() + "/salvar-wifi");
  http.setAuthorization("admin", SENHA_ADMIN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int codigo = http.POST("ssid=" + codificarUrl(ssid) + "&senha=" + codificarUrl(senha));
  http.end();
  return codigo == HTTP_CODE_OK;
}

// Envia o Wi-Fi novo para todas as portas que responderem e devolve o resultado em HTML.
String enviarWifiParaPortas(const char* ssid, const char* senha) {
  if (WiFi.status() != WL_CONNECTED) {
    return F("<p class='aviso'>A central não está conectada ao Wi-Fi atual, então não deu para "
             "enviar o Wi-Fi novo para as portas. Para trocar o Wi-Fi delas, use a rede de socorro "
             "de cada porta.</p>");
  }

  Porta portas[8];
  uint8_t qtd = buscarPortas(portas, 8);
  String r = F("<p>Envio do Wi-Fi novo para as portas:</p><ul>");
  if (qtd == 0) r += F("<li>Nenhuma porta respondeu.</li>");
  for (uint8_t i = 0; i < qtd; i++) {
    r += "<li>Porta " + String(portas[i].numero) + " (" + portas[i].ip.toString() + "): ";
    // Em outra faixa de IP ela responde à procura por broadcast, mas o HTTP não chega até ela.
    if (!naMinhaFaixa(portas[i].ip)) {
      r += F("<b>não recebeu</b>: está em outra faixa de IP. Corrija o DHCP da rede ou use a rede "
             "de socorro dela.</li>");
      continue;
    }
    bool ok = enviarWifiParaPorta(portas[i].ip, ssid, senha);
    LOG("Wi-Fi novo para a porta %u (%s): %s\n", portas[i].numero, portas[i].ip.toString().c_str(),
        ok ? "ok" : "falhou");
    r += ok ? F("recebeu</li>") : F("<b>não confirmou</b> (a senha de admin dela é a mesma da central?)</li>");
  }
  r += F("</ul><p class='aviso'>Porta que não aparece como \"recebeu\" continua no Wi-Fi antigo. "
         "Quando o Wi-Fi antigo for desligado, ela abre a rede de socorro \"Campainha-PortaN\": "
         "vá até ela com o celular, conecte nessa rede e abra http://192.168.4.1.</p>");
  return r;
}

// ===== Atualização de firmware =====

bool atualizacaoAutorizada = false;
bool atualizacaoIniciada = false;
String erroAtualizacao;
size_t marcaCasada = 0;  // quantos caracteres da marca apareceram em sequência no arquivo
bool marcaEncontrada = false;

// Procura MARCA_FIRMWARE no arquivo, pedaço por pedaço. Como o '<' só aparece no começo
// da marca, basta recomeçar a contagem quando um caractere não bate.
void procurarMarca(const uint8_t* dados, size_t tamanho) {
  const size_t tamMarca = sizeof(MARCA_FIRMWARE) - 1;
  for (size_t i = 0; i < tamanho && !marcaEncontrada; i++) {
    if (dados[i] == (uint8_t)MARCA_FIRMWARE[marcaCasada]) {
      marcaEncontrada = ++marcaCasada == tamMarca;
    } else {
      marcaCasada = dados[i] == (uint8_t)MARCA_FIRMWARE[0] ? 1 : 0;
    }
  }
}

// Recebe o arquivo em pedaços enquanto ele chega. A página com o resultado é enviada
// depois, por tratarAtualizacao().
void receberFirmware() {
  HTTPUpload& envio = servidor.upload();
  if (envio.status == UPLOAD_FILE_START) {
    atualizacaoAutorizada = servidor.authenticate("admin", SENHA_ADMIN);
    atualizacaoIniciada = false;
    erroAtualizacao = "";
    marcaCasada = 0;
    marcaEncontrada = false;
    if (!atualizacaoAutorizada) return;
    // Durante o envio o loop não roda: desliga o buzzer para ele não ficar tocando.
    if (tocando) {
      rele(false);
      tocando = false;
    }
    LOG("Recebendo firmware \"%s\"...\n", envio.filename.c_str());
    uint32_t espaco = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    atualizacaoIniciada = Update.begin(espaco);
    if (!atualizacaoIniciada) erroAtualizacao = Update.getErrorString();
  } else if (envio.status == UPLOAD_FILE_WRITE) {
    if (!atualizacaoIniciada || erroAtualizacao.length()) return;
    procurarMarca(envio.buf, envio.currentSize);
    if (Update.write(envio.buf, envio.currentSize) != envio.currentSize) {
      erroAtualizacao = Update.getErrorString();
    }
  } else if (envio.status == UPLOAD_FILE_END) {
    if (!atualizacaoIniciada || erroAtualizacao.length()) return;
    if (!marcaEncontrada) {
      erroAtualizacao = F("esse arquivo não é o firmware da central. Use o firmware.bin da pasta "
                          ".pio/build/central.");
    } else if (!Update.end(true)) {
      erroAtualizacao = Update.getErrorString();
    } else {
      LOG("Firmware gravado (%u bytes)\n", (unsigned)envio.totalSize);
    }
  } else if (envio.status == UPLOAD_FILE_ABORTED) {
    erroAtualizacao = F("o envio do arquivo foi interrompido");
  }
}

// ===== Página =====

String haQuanto(unsigned long desdeMs) {
  unsigned long s = (millis() - desdeMs) / 1000;
  if (s < 120) return "há " + String(s) + " s";
  if (s < 7200) return "há " + String(s / 60) + " min";
  if (s < 172800) return "há " + String(s / 3600) + " h";
  return "há " + String(s / 86400) + " dias";
}

String motivoDoReinicio() {
  switch (ESP.getResetInfoPtr()->reason) {
    case REASON_DEFAULT_RST: return F("ligou na energia");
    case REASON_WDT_RST: return F("<b>travou</b> (watchdog de hardware)");
    case REASON_EXCEPTION_RST: return F("<b>erro no programa</b> (exceção)");
    case REASON_SOFT_WDT_RST: return F("<b>travou</b> (watchdog de software)");
    case REASON_SOFT_RESTART: return F("reiniciada pelo programa (página, Wi-Fi novo, atualização ou falta de Wi-Fi)");
    case REASON_EXT_SYS_RST: return F("botão RST ou queda rápida de energia");
    default: return ESP.getResetReason();
  }
}

bool autorizado() {
  acessouPagina = true;
  ultimoAcessoPagina = millis();
  if (servidor.authenticate("admin", SENHA_ADMIN)) return true;
  servidor.requestAuthentication(BASIC_AUTH, "Campainha");
  return false;
}

String escapar(const String& texto) {
  String r;
  r.reserve(texto.length() + 8);
  for (size_t i = 0; i < texto.length(); i++) {
    char c = texto[i];
    switch (c) {
      case '&': r += F("&amp;"); break;
      case '<': r += F("&lt;"); break;
      case '>': r += F("&gt;"); break;
      case '"': r += F("&quot;"); break;
      case '\'': r += F("&#39;"); break;
      default: r += c;
    }
  }
  return r;
}

void enviarPagina(const String& aviso = String()) {
  bool conectado = WiFi.status() == WL_CONNECTED;
  String p;
  p.reserve(5120);
  p += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Campainha</title><style>"
         "body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 16px}"
         "fieldset{border:1px solid #ccc;border-radius:6px;margin:16px 0}"
         "label{display:block;margin-top:8px}"
         "input{display:block;width:100%;box-sizing:border-box;padding:6px;font-size:1em}"
         "input[type=checkbox]{display:inline;width:auto}"
         "input:disabled{color:#999}"
         "button{font-size:1em;padding:8px 16px;margin-top:12px;margin-right:8px}"
         ".aviso{background:#fff3cd;padding:8px;border-radius:6px}"
         "</style></head><body><h2>Campainha - central</h2>");
  if (aviso.length()) p += "<p class='aviso'>" + aviso + "</p>";

  p += F("<p>Último aviso: ");
  if (totalAvisos) {
    p += ultimaOrigem + " (" + ultimoAvisoIp.toString() + "), " + haQuanto(ultimoAvisoMs);
    if (ultimoAvisoOutraFaixa) {
      p += F(" <b>chegou por broadcast de outra faixa de IP. Tem mais de um aparelho entregando IP "
             "na rede?</b>");
    }
  } else {
    p += F("nenhum desde que ligou");
  }
  p += "<br>Avisos desde que ligou: " + String(totalAvisos);
  p += "<br>Ligada " + haQuanto(0) + "<br>Último reinício: " + motivoDoReinicio();
  p += "<br>Memória livre: " + String(ESP.getFreeHeap()) + " bytes";
  p += F("<br>Firmware: " __DATE__ " " __TIME__);
  if (conectado) {
    p += "<br>Wi-Fi: " + escapar(cfg.ssid) + " (" + String(WiFi.RSSI()) + " dBm)";
    p += "<br>IP: " + WiFi.localIP().toString() + (cfg.dhcp ? " (automático)" : " (fixo)");
    p += "<br>Gateway: " + WiFi.gatewayIP().toString() + " &middot; Máscara: " + WiFi.subnetMask().toString();
  } else {
    p += F("<br>Wi-Fi: sem conexão");
  }
  p += F("<br>Endereço por nome: http://" ENDERECO_POR_NOME);
  if (apLigado) p += F("<br>Rede de socorro \"" NOME_AP "\" ligada");
  p += F("</p><button onclick=\"fetch('/tocar?origem=teste')\">Testar buzzer</button>");

  p += F("<form method='post' action='/salvar-toque'><fieldset><legend>Toque</legend>"
         "<label>Tempo do buzzer (segundos)"
         "<input type='number' name='tempo' min='0.1' max='60' step='0.1' required value='");
  p += String(cfg.tempoToqueMs / 1000.0, 1);
  p += F("'></label><button>Salvar tempo</button></fieldset></form>");

  // No modo automático, os campos de IP fixo já vêm com o IP atual, para facilitar
  // se quiser fixar a central no IP que ela está usando.
  bool mostrarAtual = cfg.dhcp && conectado;
  IPAddress ip = mostrarAtual ? WiFi.localIP() : IPAddress(cfg.ip);
  IPAddress gateway = mostrarAtual ? WiFi.gatewayIP() : IPAddress(cfg.gateway);
  IPAddress mascara = mostrarAtual ? WiFi.subnetMask() : IPAddress(cfg.mascara);

  p += F("<form method='post' action='/salvar-rede' onsubmit='return confirmarRede(this)'>"
         "<fieldset><legend>Rede</legend>"
         "<p class='aviso'>Ao trocar o Wi-Fi aqui, a central envia o Wi-Fi novo para as portas "
         "conectadas antes de reiniciar, e mostra quais receberam.</p>"
         "<label>Nome do Wi-Fi<input name='ssid' maxlength='32' required value='");
  p += escapar(cfg.ssid);
  p += F("'></label><label>Senha do Wi-Fi<input type='password' name='senha' maxlength='63' "
         "placeholder='em branco = manter a atual'></label>"
         "<label><input type='checkbox' "
         "onchange=\"this.form.senha.type=this.checked?'text':'password'\"> mostrar senha</label>"
         "<label><input type='checkbox' name='dhcp' id='dhcp' onchange='modoIp()'");
  if (cfg.dhcp) p += F(" checked");
  p += F("> IP automático (DHCP)</label><div id='fixo'>"
         "<label>IP da central<input name='ip' required value='");
  p += ip.toString();
  p += F("'></label><label>Gateway (IP do roteador)<input name='gateway' required value='");
  p += gateway.toString();
  p += F("'></label><label>Máscara<input name='mascara' required value='");
  p += mascara.toString();
  p += F("'></label></div><button>Salvar e reiniciar</button></fieldset></form>"
         "<script>function modoIp(){var d=document.getElementById('dhcp').checked;"
         "document.querySelectorAll('#fixo input').forEach(function(e){e.disabled=d})}modoIp();"
         "function confirmarRede(f){"
         "if(f.ssid.value==f.ssid.defaultValue&&!f.senha.value)return true;"
         "return confirm('A central e as portas vão mudar para o Wi-Fi \"'+f.ssid.value+'\". "
         "Confira o nome e a senha: se estiverem errados, vai ser preciso usar a rede de socorro "
         "de cada aparelho.')}</script>");

  p += F("<fieldset><legend>Manutenção</legend>"
         "<form method='post' action='/atualizar' enctype='multipart/form-data' "
         "onsubmit=\"return confirm('Gravar o firmware novo e reiniciar a central?')\">"
         "<label>Atualizar firmware: arquivo firmware.bin da pasta .pio/build/central"
         "<input type='file' name='firmware' accept='.bin' required></label>"
         "<button>Enviar firmware</button></form>"
         "<form method='post' action='/reiniciar' onsubmit=\"return confirm('Reiniciar a central agora?')\">"
         "<button>Reiniciar a central</button></form>"
         "<form method='post' action='/restaurar' "
         "onsubmit=\"return confirm('Voltar para o Wi-Fi, IP e tempo gravados no firmware? "
         "Se o Wi-Fi mudar, as portas recebem o Wi-Fi do firmware também.')\">"
         "<button>Restaurar valores do firmware</button></form></fieldset></body></html>");

  servidor.send(200, "text/html; charset=utf-8", p);
}

// Página curta com uma mensagem (em HTML), enviada antes de reiniciar.
void enviarMensagemEReiniciar(const String& mensagem) {
  String endereco = enderecoDaCentral();
  servidor.send(200, "text/html; charset=utf-8",
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>.aviso{background:#fff3cd;padding:8px;border-radius:6px}</style></head>"
                "<body style='font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 16px'>" +
                    mensagem +
                    "<p>A central está reiniciando e volta em uns 15 segundos em "
                    "<a href='http://" + endereco + "/'>http://" + endereco + "</a></p></body></html>");
  reiniciarPedido = true;
  reiniciarPedidoEm = millis();
}

// As portas conferem que a resposta é exatamente "ok".
void tratarTocar() {
  registrarAviso(servidor.arg("origem"), servidor.client().remoteIP(), false);
  servidor.send(200, "text/plain", "ok");
}

void tratarPagina() {
  if (!autorizado()) return;
  enviarPagina();
}

void tratarSalvarToque() {
  if (!autorizado()) return;
  long ms = lroundf(servidor.arg("tempo").toFloat() * 1000);
  if (ms < 0 || !tempoValido(ms)) {
    enviarPagina(F("Tempo inválido: use de 0,1 a 60 segundos."));
    return;
  }
  cfg.tempoToqueMs = ms;
  salvarConfig();
  LOG("Tempo do toque: %lu ms\n", (unsigned long)ms);
  enviarPagina("Tempo do toque salvo: " + String(ms / 1000.0, 1) + " s.");
}

void tratarSalvarRede() {
  if (!autorizado()) return;
  String ssid = servidor.arg("ssid");
  String senha = servidor.arg("senha");
  bool dhcp = servidor.hasArg("dhcp");
  IPAddress ip, gateway, mascara;

  if (ssid.length() == 0 || ssid.length() > 32) {
    enviarPagina(F("Nome do Wi-Fi inválido (até 32 caracteres)."));
    return;
  }
  if (senha.length() > 0 && (senha.length() < 8 || senha.length() > 63)) {
    enviarPagina(F("A senha do Wi-Fi precisa ter de 8 a 63 caracteres."));
    return;
  }
  // No modo automático os campos de IP fixo nem são enviados: fica o que já estava salvo.
  if (!dhcp) {
    if (!ip.fromString(servidor.arg("ip")) || !gateway.fromString(servidor.arg("gateway")) ||
        !mascara.fromString(servidor.arg("mascara"))) {
      enviarPagina(F("IP, gateway ou máscara em formato errado (ex: 192.168.16.250). Nada foi salvo."));
      return;
    }
    if (!mascaraValida(mascara)) {
      enviarPagina(F("Máscara inválida (o normal é 255.255.255.0). Nada foi salvo."));
      return;
    }
    if (!redeValida(ip, gateway, mascara)) {
      enviarPagina(F("O IP da central tem que estar na mesma faixa do roteador e ser diferente dele "
                     "(ex: roteador 192.168.16.1, central 192.168.16.250). Nada foi salvo."));
      return;
    }
  }

  bool wifiMudou = ssid != cfg.ssid || (senha.length() && senha != cfg.senha);
  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (senha.length()) strlcpy(cfg.senha, senha.c_str(), sizeof(cfg.senha));
  cfg.dhcp = dhcp ? 1 : 0;
  if (!dhcp) {
    cfg.ip = ip;
    cfg.gateway = gateway;
    cfg.mascara = mascara;
  }

  // As portas recebem o Wi-Fi novo enquanto todos ainda estão no Wi-Fi atual.
  String resultado = wifiMudou ? enviarWifiParaPortas(cfg.ssid, cfg.senha) : String();
  salvarConfig();
  LOG("Rede salva: %s, IP %s\n", cfg.ssid, dhcp ? "automatico" : ip.toString().c_str());
  enviarMensagemEReiniciar(resultado + F("<p>Configuração salva.</p>"));
}

void tratarRestaurar() {
  if (!autorizado()) return;
  String ssidAntes = cfg.ssid;
  String senhaAntes = cfg.senha;
  carregarFabrica();
  bool wifiMudou = ssidAntes != cfg.ssid || senhaAntes != cfg.senha;

  String resultado = wifiMudou ? enviarWifiParaPortas(cfg.ssid, cfg.senha) : String();
  salvarConfig();
  LOG("Valores de fabrica restaurados\n");
  enviarMensagemEReiniciar(resultado + F("<p>Valores do firmware restaurados.</p>"));
}

void tratarReiniciar() {
  if (!autorizado()) return;
  LOG("Reinicio pedido pela pagina\n");
  enviarMensagemEReiniciar(F("<p>Reiniciando a pedido da página.</p>"));
}

// Chamado depois que o arquivo do firmware chegou inteiro (ver receberFirmware).
void tratarAtualizacao() {
  if (!autorizado()) return;
  if (!atualizacaoIniciada && erroAtualizacao.length() == 0) erroAtualizacao = F("nenhum arquivo recebido.");
  if (erroAtualizacao.length() == 0) {
    enviarMensagemEReiniciar(F("<p>Firmware gravado. Confira a data do firmware na página depois que ela voltar.</p>"));
  } else {
    // Reinicia mesmo com erro, para descartar o que foi recebido pela metade.
    enviarMensagemEReiniciar("<p><b>Firmware não foi gravado:</b> " + escapar(erroAtualizacao) +
                             " A central continua com o firmware atual.</p>");
  }
}

// ===== Programa =====

void setup() {
#ifdef RELE_VIA_SERIAL
  Serial.begin(RELE_BAUD);
  rele(false);
#else
  Serial.begin(115200);
  rele(false);  // define o nível de "desligado" antes de virar saída, para o relé não estalar
  pinMode(PINO_RELE, OUTPUT);
#endif

  EEPROM.begin(sizeof(Config));
  carregarConfig();

  WiFi.persistent(false);  // a config do Wi-Fi fica só na nossa EEPROM
  WiFi.mode(WIFI_STA);
  WiFi.hostname(NOME_NA_REDE);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // sem economia de energia: responde mais rápido
  WiFi.setAutoReconnect(true);
  conectarNaRede();
  LOG("\nCentral - conectando em \"%s\" (IP %s), toque de %lu ms...\n", cfg.ssid,
      cfg.dhcp ? "automatico" : IPAddress(cfg.ip).toString().c_str(), (unsigned long)cfg.tempoToqueMs);

  servidor.on("/", HTTP_GET, tratarPagina);
  servidor.on("/tocar", tratarTocar);
  servidor.on("/salvar-toque", HTTP_POST, tratarSalvarToque);
  servidor.on("/salvar-rede", HTTP_POST, tratarSalvarRede);
  servidor.on("/restaurar", HTTP_POST, tratarRestaurar);
  servidor.on("/reiniciar", HTTP_POST, tratarReiniciar);
  servidor.on("/atualizar", HTTP_POST, tratarAtualizacao, receberFirmware);
  servidor.onNotFound([]() { servidor.send(404, "text/plain", "nao encontrado"); });
  servidor.begin();

  udpDescoberta.begin(PORTA_DESCOBERTA);
  MDNS.begin(NOME_NA_REDE);  // se reinicia sozinho quando o Wi-Fi conecta ou muda
  MDNS.addService("http", "tcp", 80);
}

void loop() {
  servidor.handleClient();
  responderDescoberta();
  MDNS.update();
  atualizarToque();
  gerenciarWifi();

  // Espera um pouco antes de reiniciar para a página de confirmação chegar no navegador.
  if (reiniciarPedido && millis() - reiniciarPedidoEm > 1500) {
    rele(false);
    ESP.restart();
  }
}
