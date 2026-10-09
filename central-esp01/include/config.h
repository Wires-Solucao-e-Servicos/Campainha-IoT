#pragma once

// ===== Nome na rede =====
// A página da central abre em http://campainha-central.local (Windows, iPhone, Mac;
// Android antigo pode não reconhecer, aí use o IP que aparece no roteador principal).
// Esse nome também aparece na lista de aparelhos do roteador.
// Não use "espcampainha": esse é o nome do D-Link DIR-615.
#define NOME_NA_REDE "campainha-central"

// ===== Valores de fábrica =====
// O Wi-Fi, o IP e o tempo do toque podem ser trocados pela página da central.
// O que for salvo lá fica guardado na flash e vale mais que os valores daqui.
// Se você mudar algum destes valores e gravar o firmware de novo, os valores
// daqui voltam a valer.

// Wi-Fi (tem que ser o mesmo das NodeMCU)
#define WIFI_SSID  "EspCampainha"
#define WIFI_SENHA "Wires01#"

// IP automático (DHCP): as NodeMCU encontram a central sozinhas, não importa o IP.
// Com false, a central usa o IP fixo abaixo. Como o DIR-615 está em modo Access Point,
// quem distribui os IPs é o roteador principal: os três primeiros números têm que ser
// iguais aos dele, e o gateway é o IP dele.
#define USAR_DHCP  true
#define IP_CENTRAL "192.168.0.250"
#define IP_GATEWAY "192.168.0.1"
#define IP_MASCARA "255.255.255.0"

// Tempo que o buzzer fica ligado. Apertos durante o toque são ignorados.
#define TEMPO_TOQUE_MS 3000

// ===== Acesso =====
// Senha da página de configuração (usuário "admin") e da rede de socorro
// "Campainha-Central". De 8 a 63 caracteres. Tem que ser IGUAL ao SENHA_ADMIN das
// NodeMCU: é com ela que a central envia o Wi-Fi novo para as portas.
#define SENHA_ADMIN "Wires01#"

// ===== Módulo relé =====
// Há dois tipos comuns de módulo relé para ESP-01. Deixe ativo só um:
//
//   RELE_VIA_GPIO   -> o ESP-01 liga o relé direto por um pino.
//                      Ex: "ESP-01S Relay v1.0" (placa pequena, só o relé e um transistor).
//   RELE_VIA_SERIAL -> o ESP-01 manda comandos pela serial para um chip da placa.
//                      Ex: LC Technology "ESP8266 5V WiFi Relay" (tem um chip STC15F104 perto do relé).
#define RELE_VIA_GPIO
// #define RELE_VIA_SERIAL

// Modo GPIO
#define PINO_RELE          0     // GPIO0 na maioria das placas
#define RELE_ATIVO_EM_LOW  true  // se o relé ficar ligado em repouso e desligar no toque, troque para false

// Modo serial (neste modo o monitor serial fica sem mensagens, a serial é do relé)
#define RELE_BAUD 9600  // algumas versões da placa usam 115200
