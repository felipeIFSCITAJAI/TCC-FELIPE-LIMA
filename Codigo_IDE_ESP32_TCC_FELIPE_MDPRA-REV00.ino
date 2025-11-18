#include <Wire.h> // Biblioteca para comunicação I2C (usada pelo LCD, PCF8574, AS5600)
#include <LiquidCrystal_I2C.h> // Biblioteca para o display LCD I2C
#include <SPI.h> // Biblioteca para comunicação SPI (usada pelo cartão SD)
#include <SD.h> // Biblioteca para manipulação do cartão SD
#include <OneWire.h> // Biblioteca para o protocolo OneWire (sensor de temperatura)
#include <DallasTemperature.h> // Biblioteca para o sensor de temperatura DS18B20
#include <Preferences.h> // Biblioteca para salvar dados na memória NVS (não-volátil) do ESP32
#include <WiFi.h> // Biblioteca para conectividade Wi-Fi
#include <time.h> // Biblioteca para gerenciamento de data e hora (NTP)
// --- INÍCIO INTEGRAÇÃO GOOGLE SHEETS ---
#include <HTTPClient.h> // Para realizar requisições HTTP (enviar dados)
#include <ArduinoJson.h> // Para formatar os dados em JSON para envio
#include <vector> // Usado para criar listas de dados para o Google Sheets
// URL do Google Apps Script Web App
const char* googleScriptURL = "https://script.google.com/macros/s/AKfycbyriKh-rHMb274_ru7mHEv2KIuPM97dHpGmL6SvlkY7hNkWb6sTqd6lS_5a5Q1cwzMD/exec";
// Variável para armazenar o nome da aba do scan atual
String currentScanSheetName = ""; // Ex: "Scan 01", "Scan 02"
// Declarações das funções do Google Sheets (protótipos)
String seguirRedirecionamento(String url); // Função para lidar com redirecionamentos HTTP
bool escreverEmLista(String identificacao, const std::vector<String>& dados); // Envia uma linha de dados (Data, Hora, Ang, RF)
bool escreverEmCelula(String identificacao, String celula, String dado); // Escreve um valor único em uma célula específica
void montarCabecalho(String _boardID, const String& colunaInicial, const std::vector<String>& cabecalhos); // Cria o cabeçalho na planilha (A1="Data", B1="Hora", ...)
// --- FIM INTEGRAÇÃO GOOGLE SHEETS --- //



// DS18B20.
#define ONE_WIRE_BUS 4 // Pino GPIO para o sensor de temperatura DS18B20
// COOLER
#define COOLER_PIN 5 // Pino GPIO para acionar o cooler
// Driver Motor de Passo
#define MOTOR_EN_PIN 2 // Pino Enable (habilita/desabilita) do driver A4988/DRV8825
#define MOTOR_STEP_PIN 16 // Pino Step (envia pulsos) do driver
#define MOTOR_DIR_PIN 17 // Pino Direction (define direção) do driver
// Sensor RF
#define RF_SENSOR_PIN 34 // Pino de entrada analógica (ADC) para o sensor de potência RF
// Buzzer
#define BUZZER_PIN 25 // Pino GPIO para o buzzer
// Cartão SD
#define SD_MISO 19 // Pinos SPI para o cartão SD
#define SD_MOSI 23
#define SD_SCK 18
#define SD_CS 15
// AS5600
#define AS5600_ADDR 0x36 // Endereço I2C do encoder magnético AS5600
#define AS5600_ANGLE_HIGH 0x0E // Registrador do ângulo (byte alto)
#define AS5600_ANGLE_LOW 0x0F // Registrador do ângulo (byte baixo)
// WiFi fixo
#define WIFI_SSID "IFSC-PESQUISA" // Nome da rede Wi-Fi
#define WIFI_PASS "i5s4pes7" // Senha da rede Wi-Fi
// PCF8574 teclado
#define PCF_ADDR 0x20 // Endereço I2C do expansor de portas PCF8574
#define ROW1 (1 << 7) // Mapeamento dos bits do PCF para as linhas do teclado
#define ROW2 (1 << 6)
#define ROW3 (1 << 5)
#define ROW4 (1 << 4)
#define COL1 (1 << 3) // Mapeamento dos bits do PCF para as colunas do teclado
#define COL2 (1 << 2)
#define COL3 (1 << 1)
#define COL4 (1 << 0)
// Objetos globais
LiquidCrystal_I2C lcd(0x27, 16, 2); // Instância do objeto LCD
OneWire oneWire(ONE_WIRE_BUS); // Instância do barramento OneWire
DallasTemperature sensors(&oneWire); // Instância do sensor de temperatura

const char keys[4][4] = { // Matriz de layout do teclado 4x4
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
unsigned long lastDebounceTime = 0; // Variável para controle de debounce do teclado

const unsigned long debounceDelay = 50; // Tempo de espera (ms) para o debounce
char lastKeyPressed = '\0'; // Armazena a última tecla para evitar repetições
// Variáveis para controle de temperatura
unsigned long ultimaVerificacaoTemp = 0; // Temporizador para a leitura do sensor
const unsigned long intervaloVerificacaoTemp = 60000; // Intervalo (1 minuto) para verificar a temperatura
float temperaturaAtual = 0.0; // Armazena a última temperatura lida
const float TEMP_LIMITE_COOLER = 40.0; // Temperatura (em °C) para acionar o cooler
// Variáveis para controle de data (simulada, caso NTP falhe)
int diaAtual = 11;
int mesAtual = 9;
int anoAtual = 2025;
unsigned long ultimaAtualizacaoData = 0; // Temporizador para a data simulada
const unsigned long INTERVALO_DIA_SIMULADO = 86400000; // 24 horas em milissegundos
// Variáveis GLOBAIS que armazenam as configurações do NVS
Preferences preferences; // Objeto para acessar a NVS
float anguloTotal = 180.0; // Amplitude total do scan (salvo na NVS)
float anguloPasso = 5.0; // Resolução angular do scan (salvo na NVS)
int leiturasPA = 1; // Número de leituras por ângulo (salvo na NVS)
float zeroPonto = 0.0; // Posição absoluta do encoder definida como "0.0 relativo" (salvo na NVS)
int scanNumeroAtual = 0; // Contador de scans realizados (salvo na NVS)
float rfFilterValue = 0.0; // Limiar de ruído RF (salvo na NVS)
bool buzzerMuted = false; // Estado do buzzer (salvo na NVS)
bool scanAtivo = false; // Flag principal que controla a máquina de estados (loop)
String nomeArquivo = ""; // Nome do arquivo CSV no cartão SD
bool wifiConectado = false; // Flag de status do Wi-Fi
// --- NOVO: Variáveis para o novo filtro de ângulo ---
float anguloFiltradoGlobal = -1.0; // Armazena o valor do ângulo com filtro de suavização
// Variável para controle de sentido único do scan
bool scanSentidoHorario = true; // (Parece não ser usada, o scan é sempre horário)
// Variáveis para controle de torção.
float totalRotacaoHorariaScan = 0.0; // Acumula o total de graus girados no sentido horário
float totalRotacaoAntiHorariaScan = 0.0; // Acumula o total de graus girados no sentido anti-horário
// Variável auxiliar para rastrear a última posição angular conhecida.
float ultimoAnguloRastreado = -1.0; // Usada pela função rastrearMovimento()

// Declarações de funções (Protótipos)
void mostrarMenuPrincipal();
void processarTeclaMenuPrincipal(char tecla);
void iniciarScanSalvo();
void configurarScanPersonalizado();
void finalizarScan();
void executarScan();
void definirPontoZero();
bool irParaPontoZero();
bool moverMotorParaAnguloPreciso(float anguloAlvo, bool mostrarNoLCD);
void moverMotorPasso(bool sentidoHorario, int velocidade);
void mostrarMenuConfiguracoes();
void processarTeclaMenuConfiguracoes(char tecla);
void mostrarMenuMaisOpcoes();
void processarTeclaMenuMaisOpcoes(char tecla);
void mostrarMenuDadosRF();
void processarTeclaMenuDadosRF(char tecla);
void mostrarStatusRF();
void definirFiltroRF();
void mostrarMenuContScan();
void mostrarStatusTemperatura();
void mostrarStatusWifi();
void mostrarMenuBuzzer();
void cancelarScanHard();
// Funções do Buzzer
void playTone(int freq, int duration);
void playScanStartSound();
void playScanEndSound();
void playSDErrorSound();
void playAngleNotFoundSound();
void playGenericErrorSound();
void playConfirmationSound();
void playMuteToggleSound();
// ----- Estados do scan -----
enum EstadoScan { // Define a máquina de estados do scan
  POSICIONANDO, // Movendo para o próximo ângulo
  ESTABILIZANDO, // Pausa após o movimento
  COLETANDO_DADOS, // Lendo o sensor RF
  AVANCANDO // Calculando o próximo ângulo
};
EstadoScan estadoAtual = POSICIONANDO; // Estado inicial
bool primeiraExecucao = true; // Flag para o primeiro movimento (ir para 0.0)
float anguloDestino = 0.0; // Próximo ângulo alvo
// --------- Funções para controle de data e nome de arquivo  ---------
void atualizarDataSimulada() {
    time_t now;
struct tm timeinfo;
    bool ntp_ok = getLocalTime(&timeinfo); // Tenta pegar a hora da internet (NTP)
    if (ntp_ok) { // Se conseguir...
        diaAtual = timeinfo.tm_mday;
mesAtual = timeinfo.tm_mon + 1; // (tm_mon é 0-11)
        anoAtual = timeinfo.tm_year + 1900; // (tm_year é anos desde 1900)
        preferences.putInt("diaAtual", diaAtual); // Salva a data atualizada na NVS
        preferences.putInt("mesAtual", mesAtual);
        preferences.putInt("anoAtual", anoAtual);
} else { // Se NTP falhar (sem WiFi)...
        if (millis() - ultimaAtualizacaoData >= INTERVALO_DIA_SIMULADO) { // Verifica se passaram 24h
            diaAtual++; // Avança o dia (lógica simples de calendário)
if (diaAtual > 30) {
                diaAtual = 1;
mesAtual++;
                if (mesAtual > 12) {
                    mesAtual = 1;
anoAtual++;
                }
            }
            ultimaAtualizacaoData = millis(); // Reseta o temporizador de 24h
}
    }
}

// Formato do nome do arquivo
String obterNomeArquivo() {
    char fileNameBuffer[60]; // Buffer para montar o nome
    time_t now;
struct tm timeinfo;
    if (getLocalTime(&timeinfo)) { // Se tiver hora NTP...
        sprintf(fileNameBuffer, "/SCAN_%02d_%02d_%02d_%04d.csv", // Formato: /SCAN_NUM_DD_MM_AAAA.csv
                scanNumeroAtual,
                timeinfo.tm_mday,
                timeinfo.tm_mon + 1,
                timeinfo.tm_year + 1900);
} else { // Se não tiver hora NTP...
        atualizarDataSimulada(); // Garante que a data simulada está correta
sprintf(fileNameBuffer, "/SCAN_%02d_%02d_%02d_%04d_NO_TIME.csv", // Adiciona "_NO_TIME"
                scanNumeroAtual,
                diaAtual,
                mesAtual,
                anoAtual);
}
    return String(fileNameBuffer); // Retorna o nome formatado
}


// --- INÍCIO INTEGRAÇÃO GOOGLE SHEETS ---
String seguirRedirecionamento(String url) { // Lida com respostas HTTP 301/302 (redirecionamento)
  HTTPClient http;
  http.begin(url);
int httpCode = http.GET(); // Faz a requisição

  if (httpCode == HTTP_CODE_MOVED_PERMANENTLY || httpCode == HTTP_CODE_FOUND) { // Se for redirecionado...
    String newUrl = http.getLocation(); // Pega a nova URL
http.end();
    return seguirRedirecionamento(newUrl); // Chama a si mesma (recursão) com a nova URL
  } else {
    String response = http.getString(); // Se não for, pega a resposta final
    http.end();
    return response;
}
}

// Função escreverEmLista para aceitar std::vector<String>
bool escreverEmLista(String identificacao, const std::vector<String>& dados) { // Envia uma linha de dados
  if (WiFi.status() != WL_CONNECTED) { // Verifica conexão
    return false;
}

  bool flag_envio = false; // Flag de sucesso
  String url = googleScriptURL;
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json"); // Define o tipo de conteúdo
  StaticJsonDocument<512> jsonDoc; // Cria o objeto JSON
  jsonDoc["action"] = "escreverEmLista"; // Ação para o Apps Script
jsonDoc["identificacao"] = identificacao; // Nome da aba (ex: "Scan 01")
  JsonArray jsonDados = jsonDoc.createNestedArray("dados"); // Cria um array JSON
  for (const String& item : dados) { // Adiciona os dados (Data, Hora, Ang, RF) ao array
    jsonDados.add(item);
}

  String jsonString;
  serializeJson(jsonDoc, jsonString); // Converte o objeto JSON em string
  int httpResponseCode = http.POST(jsonString); // Envia o JSON via POST
if (httpResponseCode == HTTP_CODE_MOVED_PERMANENTLY || httpResponseCode == HTTP_CODE_FOUND) { // Lida com redirecionamento
    String newUrl = http.getLocation();
    http.end();
String finalResponse = seguirRedirecionamento(newUrl); // Pega a resposta final
    if (finalResponse.indexOf("Dados salvos com sucesso") != -1) { // Verifica a resposta
        flag_envio = true;
}
  } else if (httpResponseCode > 0) { // Se não foi redirecionado
    String response = http.getString();
    http.end();
if (response.indexOf("Dados salvos com sucesso") != -1) { // Verifica a resposta
        flag_envio = true;
}
  } else {
    http.end();
  }
  return flag_envio; // Retorna true se sucesso
}

bool escreverEmCelula(String identificacao, String celula, String dado) { // Escreve em uma célula específica
  if (WiFi.status() != WL_CONNECTED) { // Verifica WiFi
    return false;
}
  bool flag_envio = false;
  String url = googleScriptURL;
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<200> jsonDoc; // JSON menor
  jsonDoc["action"] = "escreverEmCelula"; // Ação para o Apps Script
jsonDoc["identificacao"] = identificacao; // Nome da aba
  jsonDoc["celula"] = celula; // Célula alvo (ex: "A1")
  jsonDoc["dado"] = dado; // Valor a ser escrito
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  int httpResponseCode = http.POST(jsonString); // Envia via POST
if (httpResponseCode == HTTP_CODE_MOVED_PERMANENTLY || httpResponseCode == HTTP_CODE_FOUND) { // Lida com redirecionamento
    String newUrl = http.getLocation();
    http.end();
    seguirRedirecionamento(newUrl);
flag_envio = true; // Assume sucesso no redirecionamento (pode ser melhorado)
  } else if (httpResponseCode > 0) {
    http.end();
    flag_envio = true; // Assume sucesso
} else {
    http.end();
  }
  return flag_envio;
}

String lerCelula(String identificacao, String celula) { // Lê o valor de uma célula
  if (WiFi.status() != WL_CONNECTED) { // Verifica WiFi
    return "Erro: WiFi desconectado";
}
  String url = googleScriptURL;
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<200> jsonDoc;
  jsonDoc["action"] = "lerCelula"; // Ação para o Apps Script
  jsonDoc["identificacao"] = identificacao;
jsonDoc["celula"] = celula;
  String jsonString;
  serializeJson(jsonDoc, jsonString);

  int httpResponseCode = http.POST(jsonString); // Envia via POST
if (httpResponseCode == HTTP_CODE_MOVED_PERMANENTLY || httpResponseCode == HTTP_CODE_FOUND) { // Lida com redirecionamento
    String newUrl = http.getLocation();
    http.end();
    return seguirRedirecionamento(newUrl); // Retorna a resposta final
} else if (httpResponseCode > 0) {
    String response = http.getString(); // Pega a resposta
    http.end();
    return response; // Retorna o conteúdo da célula
} else {
    http.end();
    return "Erro ao ler célula";
}
}

void montarCabecalho(String _boardID, const String& colunaInicial, const std::vector<String>& cabecalhos) { // Escreve o cabeçalho (A1, B1, ...)
  if (WiFi.status() != WL_CONNECTED) {
    return;
}
  String celulaVerificacao = colunaInicial + "1"; // Ex: "A1"
  String celula = lerCelula(_boardID, celulaVerificacao); // Lê o conteúdo da A1
if (celula.indexOf(cabecalhos[0]) == -1 && celula.indexOf("Erro") == -1) { // Se A1 não for "Data" (e não der erro)...
    char coluna = colunaInicial[0]; // Começa em 'A'
for (size_t i = 0; i < cabecalhos.size(); i++) { // Itera pelos cabeçalhos ("Data", "Hora", ...)
      String celulaAlvo = String(coluna) + "1"; // "A1", depois "B1", "C1", ...
int tentativas = 0;
      while (escreverEmCelula(_boardID, celulaAlvo, cabecalhos[i]) == 0 && tentativas < 5) { // Tenta escrever o cabeçalho
          delay(100); // Tenta 5 vezes
tentativas++;
      }
      coluna++; // Avança para a próxima letra (coluna)
    }
  }
}

// --- FIM INTEGRAÇÃO GOOGLE SHEETS ---

// --- Funções do Buzzer ---
void playTone(int freq, int duration) { // Função base para tocar um som
  if (!buzzerMuted) { // Só toca se o buzzer não estiver mutado
        tone(BUZZER_PIN, freq, duration);
delay(duration);
        noTone(BUZZER_PIN);
    }
}
// Funções de conveniência para sons específicos
void playScanStartSound() { playTone(1000, 600); delay(300); playTone(1200, 600); }
void playScanEndSound() { playTone(1200, 600); delay(300); playTone(1000, 600);
}
void playSDErrorSound() { playTone(500, 900); delay(300); playTone(400, 900); }
void playAngleNotFoundSound() { playTone(700, 600); delay(300); playTone(600, 600); delay(300); playTone(500, 600);
}
void playGenericErrorSound() { playTone(300, 1200); }
void playConfirmationSound() { playTone(1500, 480); }
void playMuteToggleSound() { playTone(800, 300); delay(180); playTone(900, 300);
}
// --- Fim Funções do Buzzer ---

void rastrearMovimento() { // Função para controle de torção dos cabos
    // Se a variável de rastreio ainda não foi inicializada,
    // define a posição atual como ponto de partida e retorna.
if (ultimoAnguloRastreado < 0) { // Se for a primeira execução (ou início do scan)
        ultimoAnguloRastreado = converterParaAnguloRelativo(getAnguloFiltrado()); // Define o ponto inicial
        return;
}

    float anguloAtual = converterParaAnguloRelativo(getAnguloFiltrado()); // Pega o ângulo atual
    // Calcula a diferença desde a última medição
    float diff = anguloAtual - ultimoAnguloRastreado;
// Corrige a descontinuidade do ângulo ao passar por 0/360 graus
    // Se a diferença for > 180, significa que o movimento real foi na direção oposta
    if (diff > 180.0) { // Ex: pulou de 359° para 1° (diff = -358)
        diff -= 360.0; // Correção: diff = 2°
// Movimento efetivo foi anti-horário
    } else if (diff < -180.0) { // Ex: pulou de 1° para 359° (diff = 358)
        diff += 360.0; // Correção: diff = -2°
// Movimento efetivo foi horário
    }

    // Acumula o movimento apenas se for significativo para evitar ruído do sensor
    if (abs(diff) > 0.05) { // Ignora micro-vibrações
        if (diff > 0) {
            // Diferença positiva: movimento horário
            totalRotacaoHorariaScan += diff; // Acumula no contador horário
} else {
            // Diferença negativa: movimento anti-horário
            totalRotacaoAntiHorariaScan += abs(diff); // Acumula no contador anti-horário
}
        // Atualiza o último ângulo conhecido para o próximo cálculo
        ultimoAnguloRastreado = anguloAtual;
}
}

// ----------------- Verificar Temperatura -----------------
void verificarTemperatura() {
  sensors.requestTemperatures(); // Solicita a leitura ao sensor
  temperaturaAtual = sensors.getTempCByIndex(0); // Pega o valor em °C
  digitalWrite(COOLER_PIN, temperaturaAtual > TEMP_LIMITE_COOLER ? LOW : HIGH); // Se temp > limite, liga cooler (LOW = Ligado)
ultimaVerificacaoTemp = millis(); // Reseta o temporizador de verificação
}

// ----------------- Teclado -----------------
void writePCF(uint8_t data) { // Envia dados para o PCF8574
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(data);
  Wire.endTransmission();
}

uint8_t readPCF() { // Lê dados do PCF8574
  Wire.requestFrom(PCF_ADDR, 1);
return Wire.available() ? Wire.read() : 0xFF; // Retorna o byte lido ou 0xFF (erro)
}

char getKey() { // Função principal de leitura do teclado (com debounce)
  char key = '\0'; // Tecla padrão (nenhuma)
for (int row = 0; row < 4; row++) { // Varre as 4 linhas
    uint8_t rowMask = 0xFF;
switch (row) { // Define qual linha ativar (colocar em LOW)
      case 0: rowMask &= ~ROW1; break;
      case 1: rowMask &= ~ROW2; break;
case 2: rowMask &= ~ROW3; break;
      case 3: rowMask &= ~ROW4; break;
    }
    writePCF(rowMask); // Ativa a linha
    delayMicroseconds(100); // Pequena pausa
uint8_t cols = readPCF() & 0x0F; // Lê o estado das 4 colunas
    for (int col = 0; col < 4; col++) {
      if (!(cols & (1 << (3 - col)))) { // Se uma coluna estiver em LOW...
        key = keys[row][col]; // ...encontramos a tecla pressionada
break;
      }
    }
    writePCF(0xFF); // Desativa todas as linhas
    if (key != '\0') break; // Se achou, sai do loop
}
  if (key != '\0' && key != lastKeyPressed && (millis() - lastDebounceTime > debounceDelay)) { // Lógica de Debounce
    lastDebounceTime = millis(); // Reseta o temporizador do debounce
lastKeyPressed = key; // Armazena a tecla
    return key; // Retorna a tecla válida
  }
  if (key == '\0') lastKeyPressed = '\0'; // Se soltou a tecla, reseta a última tecla
  return '\0'; // Nenhuma tecla válida
}

char getRawKey() { // Leitura "crua" do teclado (sem debounce)
  char key = '\0'; // Usado para movimento contínuo do motor (menus de ajuste)
  for (int row = 0; row < 4; row++) {
    uint8_t rowMask = 0xFF;
switch (row) {
      case 0: rowMask &= ~ROW1; break;
      case 1: rowMask &= ~ROW2; break;
case 2: rowMask &= ~ROW3; break;
      case 3: rowMask &= ~ROW4; break;
    }
    writePCF(rowMask);
    delayMicroseconds(100);
uint8_t cols = readPCF() & 0x0F;
    for (int col = 0; col < 4; col++) {
      if (!(cols & (1 << (3 - col)))) {
        key = keys[row][col];
break;
      }
    }
    writePCF(0xFF);
    if (key != '\0') break;
  }
  return key; // Retorna a tecla imediatamente
}

bool checkPCF() { // Verifica se o PCF8574 está conectado
  Wire.beginTransmission(PCF_ADDR);
  if (Wire.endTransmission() != 0) { // Se `endTransmission` retornar != 0, houve erro
    playGenericErrorSound();
    return false;
  }
  return true;
}

// ----------------- AS5600 -----------------
uint8_t as5600Read(uint8_t reg) { // Função base para ler um registrador do AS5600
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg); // Define qual registrador ler
  Wire.endTransmission(false); // Envia, mas não libera o barramento I2C
  Wire.requestFrom(AS5600_ADDR, 1); // Solicita 1 byte do registrador
  return Wire.available() ? Wire.read() : 0; // Retorna o byte lido
}

bool as5600Begin() { // Verifica se o AS5600 está conectado
  Wire.beginTransmission(AS5600_ADDR);
  if (Wire.endTransmission() != 0) { // Se falhar...
    playGenericErrorSound();
    return false;
  }
  return true;
}

float as5600GetAngle() { // Lê o ângulo absoluto (0-360)
  uint8_t highByte = as5600Read(AS5600_ANGLE_HIGH); // Lê o byte alto
  uint8_t lowByte = as5600Read(AS5600_ANGLE_LOW); // Lê o byte baixo
  uint16_t rawAngle = (highByte << 8) | lowByte; // Combina os bytes (valor 0-4095)
return (rawAngle * 360.0) / 4096.0; // Converte a leitura de 12 bits para 0-360 graus
}

float getAnguloFiltrado() { // Aplica um filtro de suavização (low-pass)
    float anguloBruto = as5600GetAngle(); // Lê o sensor
if (anguloFiltradoGlobal < 0) { // Na primeira vez...
        anguloFiltradoGlobal = anguloBruto; // ...inicializa o filtro com o valor bruto
}

    float diff = anguloBruto - anguloFiltradoGlobal; // Diferença entre o valor atual e o último filtrado
if (diff > 180.0) { // Corrige a "virada" de 360->0
        diff -= 360.0;
} else if (diff < -180.0) { // Corrige a "virada" de 0->360
        diff += 360.0;
}

    float fatorSuavizacao = 0.1; // Fator do filtro (0.1 = 10% do novo valor, 90% do antigo)
    anguloFiltradoGlobal += fatorSuavizacao * diff; // Aplica o filtro
if (anguloFiltradoGlobal >= 360.0) { // Mantém o valor no range 0-360
        anguloFiltradoGlobal -= 360.0;
} else if (anguloFiltradoGlobal < 0) {
        anguloFiltradoGlobal += 360.0;
}

    return anguloFiltradoGlobal; // Retorna o ângulo suavizado
}


float converterParaAnguloRelativo(float anguloAbsoluto) { // Converte o ângulo absoluto para o relativo (baseado no Ponto Zero)
  float relativo = anguloAbsoluto - zeroPonto; // Subtrai o "zero" definido pelo usuário
if (relativo < 0) relativo += 360.0; // Garante que o resultado seja 0-360
  if (relativo >= 360.0) relativo -= 360.0;
  return relativo;
}

String entradaDadosLCD(String titulo, bool decimal, bool inteiro = false) { // Função genérica para entrada de números
  String entrada = "";
while (true) {
    lcd.clear();
    lcd.print(titulo); // Mostra o título (ex: "Amplitude SCAN:")
    lcd.setCursor(0, 1);
    lcd.print("> " + entrada); // Mostra o valor sendo digitado
    char tecla = getKey(); // Lê o teclado (com debounce)
if (tecla == '*') { // '*' = Confirmar
      if (entrada.length() == 0 && !inteiro) { // Validação simples
        lcd.clear();
lcd.print("Preencha um valor!");
        delay(1000);
        lcd.clear();
        lcd.print(titulo);
        lcd.setCursor(0, 1);
        lcd.print("> " + entrada + "            ");
continue;
      }
      return entrada; // Retorna o valor digitado
    } else if (tecla == 'A' && entrada.length() > 0) { // 'A' = Backspace
      entrada.remove(entrada.length() - 1); // Remove o último caractere
lcd.setCursor(0, 1);
      lcd.print("> " + entrada + "     "); // Atualiza o LCD
} else if ((tecla >= '0' && tecla <= '9') || (tecla == '.' && decimal && entrada.indexOf('.') == -1)) { // Se for número (ou '.' permitido)
      entrada += tecla; // Adiciona o caractere à string
lcd.setCursor(0, 1);
      lcd.print("> " + entrada + "           ");
} else if (tecla == '#' && entrada.length() == 0) { // '#' = Cancelar (só se vazio)
      return ""; // Retorna string vazia para indicar cancelamento
}
    delay(70);
  }
}

// -------- Menu principal --------
void processarTeclaMenuPrincipal(char tecla);
void mostrarMenuPrincipal() {
  scanAtivo = false; // Garante que o scan está parado
while (true) { // Loop infinito do menu principal
    lcd.clear();
    lcd.print("A: Scan Salvo");
    lcd.setCursor(0, 1);
    lcd.print("B: Scan Custom.");
    unsigned long tStart = millis();
while (millis() - tStart < 3000) { // Mostra esta tela por 3 segundos
      char tecla = getKey(); // Verifica se há entrada
if (tecla != '\0') {
        processarTeclaMenuPrincipal(tecla); // Processa a tecla
        if (scanAtivo) return; // Se 'A' ou 'B' foi pressionado, o scanAtivo=true, e sai do menu
        break; // Sai do loop de 3s
}
      if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) { // Verifica a temp em background
        verificarTemperatura();
}
      delay(100);
    }
    delay(150);

    lcd.clear();
    lcd.print("C: Configuracoes"); // Segunda tela do menu
    lcd.setCursor(0, 1);
    lcd.print("");
tStart = millis();
    while (millis() - tStart < 3000) { // Mostra por 3 segundos
      char tecla = getKey();
if (tecla != '\0') {
        processarTeclaMenuPrincipal(tecla); // Processa a tecla ('C')
        if (scanAtivo) return; // (Não deve acontecer aqui)
        break;
}
      if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) { // Verifica temp
        verificarTemperatura();
}
      delay(100);
    }
    delay(150);
}
}

void processarTeclaMenuPrincipal(char tecla) { // Roteador do menu principal
  switch (tecla) {
    case 'A': iniciarScanSalvo(); break; // Inicia scan com config da NVS
    case 'B': configurarScanPersonalizado(); break; // Abre menu de config custom
case 'C': mostrarMenuConfiguracoes(); break; // Abre menu de configurações
    default: break; // Ignora outras teclas
  }
}

// -------- SD Card ----------
bool inicializarSD() { // Tenta inicializar o cartão SD
  lcd.clear();
  lcd.print("Iniciando SD...");
SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); // Inicia o barramento SPI nos pinos definidos
  if (!SD.begin(SD_CS)) { // Tenta montar o cartão
    playSDErrorSound(); // Erro
    lcd.clear();
    lcd.print("    !!Erro!!");
    lcd.setCursor(0, 1);
lcd.print("  !CARTAO SD!");
    delay(2000);
    lcd.clear();
    lcd.print("   Verifique:");
    lcd.setCursor(0, 1);
    lcd.print(" Conexões do SD");
    delay(2500);
    return false; // Falha
}
  lcd.clear();
  lcd.print("---Cartao SD----");
  lcd.setCursor(0, 1);
  lcd.print("---CONECTADO----");
  delay(900);
  return true; // Sucesso
}
bool criarArquivoSeguro(String filename) { // Cria o arquivo CSV para o scan
  if (SD.exists(filename.c_str())) { // Se o arquivo já existe...
    if (!SD.remove(filename.c_str())) { // ...tenta removê-lo
      playSDErrorSound(); // Erro se não conseguir remover
lcd.clear();
      lcd.print("    !!Erro!!");
      lcd.setCursor(0, 1);
      lcd.print("Remover Arquivo!");
      delay(1500);
      return false;
}
  }
  File file = SD.open(filename.c_str(), FILE_WRITE); // Cria o novo arquivo
  if (!file) { // Erro se não conseguir criar
    playSDErrorSound();
    lcd.clear();
lcd.print("    !!Erro!!");
    lcd.setCursor(0, 1);
    lcd.print("Ao criar o Arq.!");
    delay(1500);
    return false;
}
  if (file.println("Data,Hora,Angulo,Potencia RF") == 0) { // Escreve o cabeçalho CSV
    playSDErrorSound(); // Erro se não conseguir escrever
    lcd.clear();
    lcd.print("    !!Erro!!");
    lcd.setCursor(0, 1);
lcd.print("!Escrita Cabec.!");
    file.close();
    delay(1500);
    return false;
  }
  file.flush(); // Garante que o cabeçalho foi escrito
  file.close(); // Fecha o arquivo
  return true; // Sucesso
}

// ----------- Configuração scan personalizado com validações melhoradas -----------
void configurarScanPersonalizado() {
  lcd.clear();
  lcd.print(" Config. Scan:");

  float anguloTotalTemp; // Variável temporária
while (true) { // Loop de validação da Amplitude
    String val1 = entradaDadosLCD("Amplitude SCAN:", true, false); // Pede o valor
    if (val1 == "") { return; // Se cancelou, retorna ao menu principal
}
    anguloTotalTemp = val1.toFloat();
    if (anguloTotalTemp > 360.0) { // Validação: não pode ser > 360
      lcd.clear();
lcd.print(" Angulo maximo:");
      lcd.setCursor(0, 1);
      lcd.print("   360 graus!");
      delay(1600);
      continue; // Pede o valor novamente
    }
    break; // Valor válido
}

  float anguloPassoTemp; // Variável temporária
  while (true) { // Loop de validação do Passo
    String val2 = entradaDadosLCD("Angulo de Passo:", true, false);
if (val2 == "") { return; } // Cancelou
    anguloPassoTemp = val2.toFloat();
if (anguloPassoTemp <= 0 || anguloPassoTemp > anguloTotalTemp) { // Validação: 0 < Passo <= Amplitude
      lcd.clear();
      lcd.print("Passo invalido!");
      lcd.setCursor(0, 1);
lcd.print("0 < Passo <= " + String(anguloTotalTemp, 1));
      delay(1600);
      continue; // Pede novamente
    }
    break; // Valor válido
  }

  int leiturasPATemp; // Variável temporária
while (true) { // Loop de validação das Leituras
    String val3 = entradaDadosLCD("Leituras/Angulo:", false, true);
    if (val3 == "") { return; // Cancelou
}
    leiturasPATemp = val3.toInt();
    if (leiturasPATemp <= 0) { // Validação: Leituras > 0
      lcd.clear();
lcd.print("!!Leituras > 0!!");
      delay(1600);
      continue; // Pede novamente
    }
    break; // Valor válido
  }

  anguloTotal = anguloTotalTemp; // Atribui os valores temporários às variáveis globais
  anguloPasso = anguloPassoTemp;
leiturasPA = leiturasPATemp;

  lcd.clear();
  lcd.print("-Salvar config?-"); // Pergunta se quer salvar na NVS
  lcd.setCursor(0, 1);
  lcd.print("1:Sim      2:Nao");
while (true) {
    char tecla = getKey();
if (tecla == '1') { // Sim
      preferences.putFloat("anguloTotal", anguloTotal); // Salva na NVS
      preferences.putFloat("anguloPasso", anguloPasso);
      preferences.putInt("leiturasPA", leiturasPA);
      preferences.end(); // Fecha a NVS
      preferences.begin("scanConfig", false); // Reabre (necessário?)
      lcd.clear();
lcd.print("!!Configuracao");
      lcd.setCursor(0, 1);
      lcd.print("         Salva!!");
      delay(1200);
      break;
} else if (tecla == '2' || tecla == '#') { // Não ou Cancelar
      lcd.clear();
      lcd.print("!!Configuracao");
      lcd.setCursor(0, 1);
lcd.print("    Descartada!!");
      delay(1200);
      break;
    }
    delay(70);
  }

  preferences.end();
  preferences.begin("scanConfig", true); // Abre em modo Read-Only para ler outras configs
zeroPonto = preferences.getFloat("zeroPonto", 0.0);
  rfFilterValue = preferences.getFloat("rfFilter", 0.0);
  buzzerMuted = preferences.getBool("buzzerMuted", false);
  preferences.end();
  preferences.begin("scanConfig", false); // Reabre em modo Read/Write
// --- PREPARAÇÃO DO SCAN ---
  scanNumeroAtual++; // Incrementa o contador de scan
  preferences.putInt("scanNumero", scanNumeroAtual); // Salva o novo número

  totalRotacaoHorariaScan = 0.0; // Zera os contadores de torção
  totalRotacaoAntiHorariaScan = 0.0;
// Reinicia o rastreador de ângulo para o início do scan
  ultimoAnguloRastreado = -1.0; 
  
  char sheetNameBuffer[20];
sprintf(sheetNameBuffer, "Scan %02d", scanNumeroAtual); // Cria o nome da aba (ex: "Scan 01")
  currentScanSheetName = String(sheetNameBuffer);

  lcd.clear();
  lcd.print("Gerando Arquivo");
  lcd.setCursor(0, 1);
  lcd.print("     Drive/SD...");
  delay(1500);
std::vector<String> sheetHeaders = {"Data", "Hora", "Angulo", "Potencia RF"}; // Define os cabeçalhos
  if (WiFi.status() == WL_CONNECTED) { // Se conectado...
      montarCabecalho(currentScanSheetName, "A", sheetHeaders); // ...cria o cabeçalho no Google Sheets
}

  nomeArquivo = obterNomeArquivo(); // Gera o nome do arquivo CSV
  if (!criarArquivoSeguro(nomeArquivo)) { // Cria o arquivo no SD
      lcd.clear();
      lcd.print("    !!Erro!!"); // Se falhar...
lcd.setCursor(0, 1);
      lcd.print("  !CARTAO SD!");
      delay(1500);
      return; // ...aborta o início do scan
  }

  digitalWrite(MOTOR_EN_PIN, LOW); // Liga o driver do motor (LOW = Ativado)
  delay(50);
  scanAtivo = true; // Seta a flag principal para iniciar o scan
  primeiraExecucao = true; // Indica que o primeiro movimento será para 0.0
  playScanStartSound(); // Toca o som de início
}

// ------------ Funções para controle de motor -----------
void moverMotorPasso(bool sentidoHorario, int velocidade) { // Dá um único passo
  digitalWrite(MOTOR_DIR_PIN, sentidoHorario); // Define a direção
  digitalWrite(MOTOR_STEP_PIN, HIGH); // Pulso HIGH
  delayMicroseconds(velocidade); // Duração do pulso (controla velocidade)
digitalWrite(MOTOR_STEP_PIN, LOW); // Pulso LOW
  delayMicroseconds(velocidade); // Intervalo entre pulsos (controla velocidade)
}

int ajustarVelocidade(float diferenca) { // Controle de velocidade P (Proporcional)
    float abs_diferenca = abs(diferenca); // Distância absoluta do alvo
if (abs_diferenca > 30.0) { // Se > 30 graus...
        return 500; // ...velocidade alta (delay baixo)
} else if (abs_diferenca > 15.0) { // 15-30 graus
        return 5000;
} else if (abs_diferenca > 7.0) { // 7-15 graus
        return 20000;
} else if (abs_diferenca > 3.0) { // 3-7 graus
        return 45000;
} else if (abs_diferenca > 1.0) { // 1-3 graus
        return 70000;
} else { // < 1 grau
        return 90000; // Velocidade muito baixa (delay alto) para precisão
}
}

bool moverMotorParaAnguloPreciso(float anguloAlvo, bool mostrarNoLCD = true) { // Função principal de posicionamento
    const float TOLERANCIA_ANGULO = 0.7; // Margem de erro (graus)
unsigned long angleSearchStartTime = millis(); // Temporizador de timeout

    while (true) { // Loop de posicionamento
        // Checagem de cancelamento e timeout
        char tecla = getKey();
if (tecla == '#') { // Se '#' for pressionado...
            cancelarScanHard(); // ...cancela o scan
            return false; // Retorna falha
}
        if (millis() - angleSearchStartTime > 60000) { // Timeout de 60 segundos
            playAngleNotFoundSound();
lcd.clear();
            lcd.print("..Angulo");
            lcd.setCursor(0, 1);
            lcd.print("nao encontrado..");
            delay(1500);
            return false; // Retorna falha
        }

        float anguloAtualRelativo = converterParaAnguloRelativo(getAnguloFiltrado()); // Lê a posição atual
// Calcula a menor distância angular entre o ponto atual e o alvo.
// O sinal desta variável determinará a direção do movimento.
        float shortest_diff = anguloAlvo - anguloAtualRelativo; // Diferença para o alvo
if (shortest_diff > 180.0) shortest_diff -= 360.0; // Encontra o caminho mais curto (ex: -20° ao invés de +340°)
        if (shortest_diff < -180.0) shortest_diff += 360.0;
// Condição de parada: se a menor distância for menor que a tolerância, o alvo foi alcançado.
if (abs(shortest_diff) <= TOLERANCIA_ANGULO) { // Se chegou...
            delay(200); // Pausa para estabilizar a vibração
// Pausa para estabilizar
            // Re-verifica após a pausa para confirmar a posição final
            anguloAtualRelativo = converterParaAnguloRelativo(getAnguloFiltrado());
shortest_diff = anguloAlvo - anguloAtualRelativo; // Recalcula a diferença
            if (shortest_diff > 180.0) shortest_diff -= 360.0;
            if (shortest_diff < -180.0) shortest_diff += 360.0;
if(abs(shortest_diff) <= TOLERANCIA_ANGULO){ // Se ainda estiver dentro da tolerância...
                 rastrearMovimento(); // Registra o movimento final (para anti-torção)
// Garante o rastreio final
                 return true; // ...sucesso!
// Posição alcançada com sucesso
            }
        }

        // --- LÓGICA DE DIREÇÃO DINÂMICA ---
        // A direção é decidida AQUI, a cada ciclo, com base no caminho mais curto.
// Se shortest_diff > 0, o alvo está no sentido horário.
// Se shortest_diff < 0, o alvo está no sentido anti-horário.
        bool sentidoMovimento = (shortest_diff > 0); // true = Horário, false = Anti-horário
// A velocidade é ajustada com base na distância absoluta (abs)
        int velocidade = ajustarVelocidade(abs(shortest_diff)); // Pega a velocidade (delay)
// O motor se move na direção recalculada
        moverMotorPasso(sentidoMovimento, velocidade); // Dá um passo
        rastrearMovimento(); // Registra o passo (para anti-torção)
if (mostrarNoLCD && (millis() % 250 < 20)) { // Atualiza o LCD (evita flickering)
            lcd.setCursor(0, 1);
lcd.print("Ang: " + String(anguloAtualRelativo, 1) + "/" + String(anguloAlvo, 1) + "   ");
}
    }
}

bool irParaPontoZero() { // Função de conveniência
    lcd.clear();
    lcd.print("Buscando Ponto 0");
    return moverMotorParaAnguloPreciso(0.0, true); // Chama a função com alvo 0.0
}

void cancelarScanHard() { // Função de cancelamento ('#')
    scanAtivo = false; // Para o loop principal
    primeiraExecucao = true; // Reseta flags
    estadoAtual = POSICIONANDO; // Reseta a máquina de estados

    playTone(600, 300); // Som de cancelamento
    delay(100);
    playTone(600, 300);
lcd.clear();
    lcd.print("--- SCAN ---");
    lcd.setCursor(0, 1);
    lcd.print("-- CANCELADO --");
    delay(1500);
}

void executarScan() { // Máquina de estados principal do scan (chamada em loop)
  static float anguloDestino = 0.0; // Ângulo alvo (estático para persistir)
char tecla = getKey();
  if (tecla == '#') { // Verifica cancelamento
    cancelarScanHard();
    return;
}
  // Na primeira execução, apenas prepara as variáveis e a tela.
// O código prosseguirá para o 'switch' para iniciar o movimento imediatamente.
if (primeiraExecucao) { // Se for o primeiro ciclo do scan...
    anguloDestino = 0.0; // ...o primeiro alvo é sempre 0.0
    estadoAtual = POSICIONANDO; // Define o estado inicial
    lcd.clear();
    lcd.print("-Iniciando SCAN-");
    lcd.setCursor(0, 1);
    lcd.print("..Posicionando.."); // (Para o Ponto 0)
}

  switch (estadoAtual) { // Início da máquina de estados
    case POSICIONANDO: // Estado: Movendo para o ângulo
      if (moverMotorParaAnguloPreciso(anguloDestino, true)) { // Tenta chegar ao alvo
        // A flag 'primeiraExecucao' só é desativada DEPOIS que o primeiro
        // posicionamento (para o ponto zero) for bem-sucedido.
if (primeiraExecucao) {
            primeiraExecucao = false; // Desativa a flag
}

        estadoAtual = ESTABILIZANDO; // Se chegou, muda o estado
        lcd.clear();
lcd.print("Angulo: " + String(anguloDestino, 1) +"       ");
        lcd.setCursor(0, 1);
        lcd.print("Estabilizando...");
        delay(1500); // Pausa de 1.5s para vibração mecânica
        estadoAtual = COLETANDO_DADOS; // Próximo estado
} else { // Se `moverMotor...` retornou `false` (cancelado ou timeout)
        if(scanAtivo) { // Se não foi cancelado por tecla (foi timeout)...
           finalizarScan(); // ...finaliza o scan
}
      }
      break;
case ESTABILIZANDO:
       // Este estado é basicamente a pausa (delay) no final do case POSICIONANDO
       estadoAtual = COLETANDO_DADOS; // Apenas avança
       break;
case COLETANDO_DADOS: { // Estado: Lendo o sensor
      lcd.setCursor(0, 1);
      lcd.print("Coletando Media...");

      long somaPotenciaRF = 0; // Acumulador para a média
      bool cancelado = false;
for (int i = 0; i < leiturasPA; i++) { // Loop (ex: 10 leituras)
        char cancelKey = getKey(); // Verifica cancelamento a cada leitura
if (cancelKey == '#') {
            cancelado = true;
            break;
}
        lcd.setCursor(0, 1);
lcd.print("Lendo: " + String(i + 1) + "/" + String(leiturasPA) + "       "); // Mostra progresso
somaPotenciaRF += analogRead(RF_SENSOR_PIN); // Lê o ADC e soma
        delay(100); // Pausa entre leituras
      }

      if (cancelado) { // Se cancelou no meio da coleta
          cancelarScanHard();
return;
      }

      int mediaPotenciaRF_raw = (leiturasPA > 0) ? (somaPotenciaRF / leiturasPA) : 0; // Calcula a média
int mediaPotenciaRF = (mediaPotenciaRF_raw < rfFilterValue) ? 0 : (mediaPotenciaRF_raw - (int)rfFilterValue); // Aplica o limiar de ruído

      float anguloBruto = as5600GetAngle(); // Pega o ângulo exato da medição
      float anguloFiltrado = getAnguloFiltrado();
float anguloRelativo = converterParaAnguloRelativo(anguloFiltrado); // Converte para relativo

      Serial.println("--- DADOS DO PONTO ---"); // Debug no Serial Monitor
      Serial.println("Angulo Bruto (Sensor): " + String(anguloBruto));
Serial.println("Angulo Filtrado (Absoluto): " + String(anguloFiltrado));
      Serial.println("Angulo Relativo (Final): " + String(anguloRelativo));
Serial.println("Media RF (Bruta): " + String(mediaPotenciaRF_raw) + ", Media RF (Final): " + String(mediaPotenciaRF));
      Serial.println("----------------------");

      time_t now;
      struct tm timeinfo;
char dateBuffer[15];
      char timeBuffer[15];
      String dataStr = "N/A";
      String horaStr = "N/A";
if (getLocalTime(&timeinfo)) { // Se tiver hora NTP...
          strftime(dateBuffer, sizeof(dateBuffer), "%d/%m/%Y", &timeinfo); // Formata data
          strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &timeinfo); // Formata hora
dataStr = String(dateBuffer);
          horaStr = String(timeBuffer);
      }

      std::vector<String> dadosParaSheets; // Prepara os dados para o Google Sheets
      dadosParaSheets.push_back(dataStr);
      dadosParaSheets.push_back(horaStr);
      dadosParaSheets.push_back(String(anguloRelativo, 1));
      dadosParaSheets.push_back(String(mediaPotenciaRF));
escreverEmLista(currentScanSheetName, dadosParaSheets); // Envia para o Google (não bloqueante)

      File file = SD.open(nomeArquivo.c_str(), FILE_APPEND); // Abre o arquivo CSV no modo "append"
      if (file) {
        String linha = dataStr + "," + horaStr + "," + // Monta a linha CSV
                       String(anguloRelativo, 1) + "," + String(mediaPotenciaRF);
file.println(linha); // Escreve no SD
        file.flush(); // Salva
        file.close(); // Fecha
      } else {
        playSDErrorSound(); // Erro no SD
}

      estadoAtual = AVANCANDO; // Próximo estado
      break;
}

    case AVANCANDO: // Estado: Calculando próximo passo
      anguloDestino += anguloPasso; // Avança o ângulo alvo (ex: 0 -> 5 -> 10)
if (anguloDestino > anguloTotal + 0.1) { // Se o próximo ângulo > Amplitude (com margem)
        finalizarScan(); // ...o scan terminou
        return;
}
      estadoAtual = POSICIONANDO; // Se não terminou, volta ao estado de posicionar
      lcd.clear();
      lcd.print("Proximo Angulo..");
      lcd.setCursor(0, 1);
      lcd.print("Angulo: " + String(anguloDestino, 1));
      break;
}
}

void finalizarScan() { // Rotina de finalização do scan
    scanAtivo = false; // Para a máquina de estados (loop)
    primeiraExecucao = true; // Reseta a flag
    playScanEndSound(); // Toca som de fim

    lcd.clear();
    lcd.print("Finalizando...");
// --- LÓGICA DE ANTI-TORÇÃO BASEADA NO SALDO DO SCAN ---
    float saldoTorcao = totalRotacaoHorariaScan - totalRotacaoAntiHorariaScan; // Calcula o "saldo" de torção
lcd.setCursor(0, 1);
    lcd.print("Saldo:" + String(saldoTorcao, 1)+"       "); // Mostra o saldo
    delay(2000);
// Só executa a correção se o saldo for significativo (maior que meio grau)
    if (abs(saldoTorcao) > 0.5) {
        bool sentidoCorrecao;
// true = horário, false = anti-horário

        if (saldoTorcao > 0) { // Se girou mais no sentido horário...
            // Saldo positivo (mais rotação horária), então a correção é anti-horária.
sentidoCorrecao = false; // ...corrige no anti-horário
            lcd.clear();
            lcd.print("Corrigindo Torc.   ");
            lcd.setCursor(0, 1);
            lcd.print("<- Anti-horario");
} else { // Se girou mais no sentido anti-horário...
            // Saldo negativo (mais rotação anti-horária), correção é horária.
sentidoCorrecao = true; // ...corrige no horário
            lcd.clear();
            lcd.print("Corrigindo Torc.   ");
            lcd.setCursor(0, 1);
            lcd.print("-> Horario");
}
        delay(1500);
        float anguloPercorridoCorrecao = 0; // Acumulador da correção
        float anguloAnterior = converterParaAnguloRelativo(getAnguloFiltrado()); // Ponto de partida
unsigned long startTime = millis();
        // Loop para mover o motor pela quantidade exata do saldo de torção
        // Timeout de 2 minutos para a correção, caso algo dê errado.
while (anguloPercorridoCorrecao < abs(saldoTorcao) && millis() - startTime < 120000) { // Loop: move até zerar o saldo
            // Usa uma velocidade constante e moderada para o retorno de correção
            moverMotorPasso(sentidoCorrecao, 800); // Move (velocidade média)
float anguloAtual = converterParaAnguloRelativo(getAnguloFiltrado());
            float diff = anguloAtual - anguloAnterior; // Calcula o delta
// Corrige a descontinuidade do ângulo para somar corretamente a distância percorrida
            if (sentidoCorrecao) { // Movimento Horário
                if (diff < -180.0) diff += 360.0; // Corrige "virada"
} else { // Movimento Anti-horário
                if (diff > 180.0) diff -= 360.0; // Corrige "virada"
}

            // Ignora saltos muito grandes que podem ser ruído do sensor
            if (abs(diff) < 10.0) {
                anguloPercorridoCorrecao += abs(diff); // Acumula o ângulo percorrido
}

            anguloAnterior = anguloAtual; // Atualiza o ângulo anterior
// Atualiza o LCD periodicamente com o progresso da correção
            if (millis() % 250 < 20) { // Evita flickering
                 lcd.setCursor(0, 1);
lcd.print(String(anguloPercorridoCorrecao, 0) + "/" + String(abs(saldoTorcao), 0) + "      "); // Mostra progresso
}
        }
    }

    // --- MODIFICAÇÃO: RETORNO AO PONTO ZERO APÓS CORREÇÃO ---
    lcd.clear();
lcd.print("Correcao OK!");
    lcd.setCursor(0, 1);
    lcd.print("Retornando a 0..");
    delay(1500);
    irParaPontoZero(); // <-- NOVA LINHA: Chama a função para retornar ao ponto inicial.
// --- FIM DA MODIFICAÇÃO ---

    // Desliga o motor após a correção
    digitalWrite(MOTOR_EN_PIN, HIGH); // Desativa o driver do motor ( economiza energia)
delay(50);

    // --- Exibição dos dados finais do Scan ---
    lcd.clear();
    lcd.print("Scan finalizado!");
    lcd.setCursor(0, 1);
    lcd.print("--Dados salvos--");
delay(1500);

    lcd.clear();
    lcd.print("Amplitude: " + String(anguloTotal, 1)+ "      "); // Mostra resumo
    lcd.setCursor(0, 1);
lcd.print("Leituras/Ang: " + String(leiturasPA)+ "      ");
    delay(2000);

    lcd.clear();
    lcd.print("--Dados Salvos--");
    lcd.setCursor(4, 1);
    lcd.print(currentScanSheetName); // Mostra o nome da aba
    delay(2000);
}

void definirPontoZero() { // Menu para ajuste manual do Ponto Zero
  lcd.clear();
  lcd.print("-Ajustar Ponto 0-");
  lcd.setCursor(0, 1);
  lcd.print("A:HOR B:ANTI");
  delay(1500);
const int velocidades[4] = {2000, 50, 10, 1}; // Velocidades de ajuste (delay em µs)
  int velocidadeAtual = 3; // Começa na mais rápida (índice 3 = delay 1)
digitalWrite(MOTOR_EN_PIN, LOW); // Liga o motor
  delay(50);

  while (true) {
    float anguloAbsoluto = getAnguloFiltrado(); // Pega o ângulo com filtro
    float anguloRelativo = converterParaAnguloRelativo(anguloAbsoluto); // Calcula o relativo

    lcd.setCursor(0, 0);
lcd.print("Relativo: " + String(anguloRelativo, 1) + "  "); // Mostra o relativo atual

    lcd.setCursor(0, 1);
lcd.print("Velocidade: " + String(velocidadeAtual + 1) + "   "); // Mostra a velocidade (1-4)

    char tecla = getRawKey(); // Lê sem debounce (movimento contínuo)
if (tecla >= '1' && tecla <= '4') { // Teclas 1-4
      velocidadeAtual = tecla - '1'; // Mudam o índice da velocidade
}

    if (tecla == 'A') { // 'A' = Horário
      moverMotorPasso(true, velocidades[velocidadeAtual]);
} else if (tecla == 'B') { // 'B' = Anti-horário
      moverMotorPasso(false, velocidades[velocidadeAtual]);
} else if (tecla == '*') { // '*' = Salvar
      zeroPonto = anguloAbsoluto; // Salva o ângulo ABSOLUTO atual como o novo Ponto Zero

      preferences.putFloat("zeroPonto", zeroPonto); // Salva na NVS
      preferences.end();
      preferences.begin("scanConfig", false);
lcd.clear();
      lcd.print(" Zero definido!");
      lcd.setCursor(0, 1);
      lcd.print("Angulo: " + String(converterParaAnguloRelativo(anguloAbsoluto), 1) + "      "); // Mostra o novo relativo (deve ser 0.0)
      delay(1500);
digitalWrite(MOTOR_EN_PIN, HIGH); // Desliga o motor
      delay(50);
      return; // Sai da função
    } else if (tecla == '#') { // '#' = Cancelar
      lcd.clear();
      lcd.print("Definicao Zero");
lcd.setCursor(0, 1);
      lcd.print("---Cancelada----");
      delay(1000);
      digitalWrite(MOTOR_EN_PIN, HIGH); // Desliga o motor
      delay(50);
      return; // Sai da função
    }
    rastrearMovimento(); // Continua rastreando o movimento manual
    delay(10);
}
}

// ============ Menu Status WiFi ============
void mostrarStatusWifi() {
  lcd.clear();
  String ssid = WiFi.SSID(); // Pega o SSID da rede conectada
if (ssid.length() == 0) ssid = "Sem rede"; // Se não houver, mostra "Sem rede"
  bool conectado = (WiFi.status() == WL_CONNECTED); // Verifica o status
while (true) {
    lcd.setCursor(0, 0);
    lcd.print("SSID:");
    lcd.setCursor(5, 0);
    lcd.print(ssid.substring(0, 11)); // Mostra os 11 primeiros caracteres do SSID
    lcd.setCursor(0, 1);
if (conectado) {
      lcd.print("---Conectado!---");
    } else {
      lcd.print("--Desconectado--");
}
    char tecla = getKey();
    if (tecla == '#') break; // '#' para voltar
    delay(200);
  }
  lcd.clear();
  lcd.print("...Retornando...");
  delay(500);
}

// --- iniciarScanSalvo ---
void iniciarScanSalvo() { // Inicia o scan com as configs da NVS
    scanNumeroAtual++; // Incrementa o contador
    preferences.putInt("scanNumero", scanNumeroAtual); // Salva o novo número
    preferences.end();
    preferences.begin("scanConfig", true); // Abre em modo Read-Only
    anguloTotal = preferences.getFloat("anguloTotal", 180.0); // Carrega config (ou usa padrão 180.0)
anguloPasso = preferences.getFloat("anguloPasso", 5.0); // Carrega config (ou usa padrão 5.0)
    leiturasPA = preferences.getInt("leiturasPA", 1); // Carrega config (ou usa padrão 1)
    zeroPonto = preferences.getFloat("zeroPonto", 0.0); // Carrega Ponto Zero
    rfFilterValue = preferences.getFloat("rfFilter", 0.0); // Carrega Limiar RF
    buzzerMuted = preferences.getBool("buzzerMuted", false); // Carrega status do Mudo
preferences.end();
    preferences.begin("scanConfig", false); // Reabre em R/W
    totalRotacaoHorariaScan = 0.0; // Zera contadores de torção
    totalRotacaoAntiHorariaScan = 0.0;
    // Reinicia o rastreador de ângulo para o início do scan
    ultimoAnguloRastreado = -1.0;
char sheetNameBuffer[20];
    sprintf(sheetNameBuffer, "Scan %02d", scanNumeroAtual); // Cria nome da aba
    currentScanSheetName = String(sheetNameBuffer);
    lcd.clear();
    lcd.print("Criando Arquivo");
    lcd.setCursor(0, 1);
    lcd.print("     (....)");
delay(1500);
    std::vector<String> sheetHeaders = {"Data", "Hora", "Angulo", "Potencia RF"}; // Cabeçalhos
    if (WiFi.status() == WL_CONNECTED) {
        montarCabecalho(currentScanSheetName, "A", sheetHeaders); // Cria cabeçalho no Google
}

    nomeArquivo = obterNomeArquivo(); // Gera nome do arquivo
    if (!criarArquivoSeguro(nomeArquivo)) { // Cria arquivo no SD
        lcd.clear();
lcd.print("    !!Erro!!"); // Aborta se falhar
        lcd.setCursor(0, 1);
        lcd.print("  !CARTAO SD!");
        delay(1500);
        return;
    }

    digitalWrite(MOTOR_EN_PIN, LOW); // Liga motor
    delay(50);
scanAtivo = true; // Seta flag para iniciar scan
    primeiraExecucao = true; // Flag para ir para 0.0
    playScanStartSound(); // Som de início
}

// --- Segundo Menu: Configurações ---
void mostrarMenuConfiguracoes() { // Loop do menu de Configurações
  while (true) {
        lcd.clear();
lcd.print("A: Ponto Zero"); // Tela 1
        lcd.setCursor(0, 1);
        lcd.print("B: Definicoes RF");
        unsigned long tStart = millis();
while (millis() - tStart < 3000) { // Mostra por 3s
            char tecla = getKey();
if (tecla != '\0') {
                if (tecla == '#') return; // '#' para voltar ao menu principal
processarTeclaMenuConfiguracoes(tecla); // Processa 'A' ou 'B'
                break;
            }
            if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) { // Temp em background
                verificarTemperatura();
}
            delay(100);
}
        delay(200);

        lcd.clear();
        lcd.print("C: Contagem SCAN"); // Tela 2
        lcd.setCursor(0, 1);
        lcd.print("D: Mais Opcoes");
tStart = millis();
        while (millis() - tStart < 3000) { // Mostra por 3s
            char tecla = getKey();
if (tecla != '\0') {
                if (tecla == '#') return; // '#' para voltar
processarTeclaMenuConfiguracoes(tecla); // Processa 'C' ou 'D'
                break;
            }
            if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) { // Temp em background
                verificarTemperatura();
}
            delay(100);
}
        delay(200);
  }
}

void processarTeclaMenuConfiguracoes(char tecla) { // Roteador do menu Config
  switch (tecla) {
        case 'A': definirPontoZero(); // Abre menu de ajuste do Ponto Zero
break;
        case 'B': mostrarMenuDadosRF(); break; // Abre menu de RF
        case 'C': mostrarMenuContScan(); break; // Abre menu de resetar contador
        case 'D': mostrarMenuMaisOpcoes(); break; // Abre submenu "Mais Opções"
        default: break;
}
}

// --- Terceiro Menu: Mais Opções ---
void mostrarMenuMaisOpcoes() { // Loop do submenu
  while (true) {
        lcd.clear();
lcd.print("A: Temperatura"); // Tela 1
        lcd.setCursor(0, 1);
        lcd.print("B: Status WiFi");
        unsigned long tStart = millis();
while (millis() - tStart < 3000) {
            char tecla = getKey();
if (tecla != '\0') {
                if (tecla == '#') return; // '#' para voltar ao menu Config
processarTeclaMenuMaisOpcoes(tecla); // Processa 'A' ou 'B'
                break;
            }
            if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) {
                verificarTemperatura();
}
            delay(100);
}
        delay(200);

        lcd.clear();
        lcd.print("C: Buzzer"); // Tela 2
        lcd.setCursor(0, 1);
        lcd.print("");
        tStart = millis();
while (millis() - tStart < 3000) {
            char tecla = getKey();
if (tecla != '\0') {
                if (tecla == '#') return; // '#' para voltar
processarTeclaMenuMaisOpcoes(tecla); // Processa 'C'
                break;
            }
            if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) {
                verificarTemperatura();
}
            delay(100);
}
        delay(200);
  }
}

void processarTeclaMenuMaisOpcoes(char tecla) { // Roteador do submenu
  switch (tecla) {
        case 'A': mostrarStatusTemperatura(); // Mostra tela de temperatura
break;
        case 'B': mostrarStatusWifi(); break; // Mostra tela de WiFi
        case 'C': mostrarMenuBuzzer(); break; // Mostra tela do Buzzer
        default: break;
}
}

// --- Menu definicoes RF ---
void mostrarMenuDadosRF() { // Loop do menu de RF
  while (true) {
        lcd.clear();
lcd.print("A: Status Pot."); // "Status Potência" (leitura ao vivo)
        lcd.setCursor(0, 1);
        lcd.print("B: Limiar Pot."); // "Limiar Potência" (definir filtro)
        unsigned long tStart = millis();
while (millis() - tStart < 3000) {
            char tecla = getKey();
if (tecla != '\0') {
                if (tecla == '#') return; // '#' para voltar ao menu Config
processarTeclaMenuDadosRF(tecla); // Processa 'A' ou 'B'
                break;
            }
            if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) {
                verificarTemperatura();
}
            delay(100);
}
        delay(200);
  }
}

void processarTeclaMenuDadosRF(char tecla) { // Roteador do menu RF
  switch (tecla) {
        case 'A': mostrarStatusRF(); // Abre leitor de RF ao vivo
break;
        case 'B': definirFiltroRF(); break; // Abre menu para definir limiar
        default: break;
  }
}

// --- Status RF ---
void mostrarStatusRF() { // Leitura de RF e ângulo ao vivo
    lcd.clear();
lcd.print("-Status Pot. RF-");
    digitalWrite(MOTOR_EN_PIN, LOW); // Liga o motor
    delay(50);
    const int MAX_SPEED_DELAY = 5; // Velocidade rápida para ajuste
while (true) {
        int potenciaRF = analogRead(RF_SENSOR_PIN); // Lê o ADC
        float anguloAtualRelativo = converterParaAnguloRelativo(getAnguloFiltrado()); // Lê o ângulo
        lcd.setCursor(0, 0);
lcd.print("Pot. RF: " + String(potenciaRF) + "       "); // Mostra potência
        lcd.setCursor(0, 1);
lcd.print("Angulo: " + String(anguloAtualRelativo, 1) + "      "); // Mostra ângulo

        char tecla = getRawKey(); // Lê sem debounce
if (tecla == 'A') { // 'A' = Horário
            moverMotorPasso(true, MAX_SPEED_DELAY);
} else if (tecla == 'B') { // 'B' = Anti-horário
            moverMotorPasso(false, MAX_SPEED_DELAY);
} else if (tecla == '*' || tecla == '#') { // '*' ou '#' para sair
            break;
}
        rastrearMovimento(); // Continua rastreando
        delay(10);
    }
    digitalWrite(MOTOR_EN_PIN, HIGH); // Desliga o motor ao sair
    delay(50);
}

// --- Definir Limiar RF ---
void definirFiltroRF() { // Menu para definir o limiar (filtro) de RF
    lcd.clear();
    lcd.print("Limiar Atual:"); // Mostra o valor atual
    lcd.setCursor(0, 1);
    lcd.print(String(rfFilterValue, 1));
    delay(2000);
String entrada = entradaDadosLCD("Nova Limiar Pot:", true, false); // Pede o novo valor
    if (entrada == "") { // Se cancelou...
        return; // ...retorna
}
    float novoFiltro = entrada.toFloat();
    if (novoFiltro < 0) { // Validação
        lcd.clear();
lcd.print("!Valor invalido!");
        delay(1000);
        return;
    }
    rfFilterValue = novoFiltro; // Atualiza a variável global
    preferences.putFloat("rfFilter", rfFilterValue); // Salva na NVS
    preferences.end();
    preferences.begin("scanConfig", false);

    lcd.clear();
    lcd.print("-Limiar Salva!-");
lcd.setCursor(0, 1);
    lcd.print("Pot: " + String(rfFilterValue, 1)); // Mostra o novo valor
    delay(1500);
}

// --- Contagem SCAN ---
void mostrarMenuContScan() { // Menu para ver e resetar o contador
    lcd.clear();
lcd.print("Cont. Atual: " + String(scanNumeroAtual)); // Mostra o número atual
    lcd.setCursor(0, 1);
    lcd.print("Zerar Contagem?");

  while (true) {
        char tecla = getKey();
if (tecla == '*') { // '*' = Zerar
            lcd.clear();
            lcd.print("Confirmar Reset?"); // Pede confirmação
lcd.setCursor(0, 1);
            lcd.print("(*=Sim)  (#=Nao)");
            playConfirmationSound();
            char confirmTecla = '\0';
            unsigned long confirmStartTime = millis();
const unsigned long confirmTimeout = 5000; // Timeout de 5 segundos
            while (confirmTecla == '\0' && (millis() - confirmStartTime < confirmTimeout)) { // Espera 5s
                confirmTecla = getKey();
delay(50);
            }

            if (confirmTecla == '*') { // Se confirmou ('*')
                scanNumeroAtual = 0; // Zera a variável
preferences.putInt("scanNumero", scanNumeroAtual); // Salva 0 na NVS
                preferences.end();
                preferences.begin("scanConfig", false);
                playConfirmationSound();
                lcd.clear();
                lcd.print("Contagem zerada!");
                delay(1500);
                return; // Volta ao menu anterior
} else { // Se não confirmou ('#' ou timeout)
                lcd.clear();
lcd.print("Cont. Atual: " + String(scanNumeroAtual));
                lcd.setCursor(0, 1);
                lcd.print("Zerar Contagem?");
                
                if (confirmTecla == '#') { // Se apertou '#' (Cancelar)
                    lcd.clear();
lcd.print("---Cancelado!---");
                    delay(1000);
                    return; // Volta
                } else { // Se deu timeout
                    lcd.clear();
lcd.print("!Tempo esgotado!");
                    delay(1000);
                    return; // Volta
                }
              }
            } else if (tecla == '#') { // Se apertou '#' na primeira tela
              return; // Volta
}
        delay(70);
  }
}

// --- Status Temperatura ---
void mostrarStatusTemperatura() { // Tela de status da temperatura
  unsigned long lastDisplayUpdateTime = 0; // Temporizador para evitar flickering
const unsigned long displayUpdateInterval = 500; // Atualiza a cada 500ms
  while (true) {
        char tecla = getKey();
if (tecla == '#') { // '#' para voltar
            return;
}

        if (millis() - lastDisplayUpdateTime >= displayUpdateInterval) { // Se passaram 500ms
            verificarTemperatura(); // Lê o sensor (e controla o cooler)
lcd.clear();
            lcd.print("Temperatura: " + String(temperaturaAtual, 1) + " C"); // Mostra temp
            lcd.setCursor(0, 1);
lcd.print("Cooler: " + String(digitalRead(COOLER_PIN) == LOW ? "Ligado" : "Desl.")); // Mostra status (LOW=Ligado)
            lastDisplayUpdateTime = millis(); // Reseta o temporizador
}
        delay(50);
  }
}

// --- Menu Buzzer ---
void mostrarMenuBuzzer() { // Menu para Mutar/Desmutar
   bool needsRedraw = true; // Flag para redesenhar a tela
while (true) {
      if (needsRedraw) { // Só redesenha se necessário
      lcd.clear();
lcd.print("Buzzer: " + String(buzzerMuted ? "Mutado" : "Ligado")); // Mostra status atual
      lcd.setCursor(0, 1);
      lcd.print("*=Mutar #=Voltar"); 
      needsRedraw = false; // Tela desenhada
// A tela foi desenhada, então resetamos a flag.
    }
        char tecla = getKey();
if (tecla == '*') { // '*' = Inverter Mudo
            buzzerMuted = !buzzerMuted; // Inverte a flag
preferences.putBool("buzzerMuted", buzzerMuted); // Salva o novo estado na NVS
            preferences.end();
            preferences.begin("scanConfig", false);
            playMuteToggleSound(); // Toca som de confirmação
            lcd.clear();
            lcd.print("Buzzer " + String(buzzerMuted ? "Mutado!" : "Ligado!"));
            delay(1500);
            needsRedraw = true; // Força redesenhar a tela do menu
} else if (tecla == '#') { // '#' = Voltar
            return;
}
        delay(70);
  }
}

// --------------- Setup ---------------
void setup() {
  Serial.begin(115200); // Inicia comunicação serial (debug)
  Wire.begin(); // Inicia barramento I2C
  lcd.init(); // Inicia LCD
  lcd.backlight(); // Liga a luz de fundo
pinMode(BUZZER_PIN, OUTPUT); // Define pino do buzzer como saída

  lcd.clear();
  lcd.setCursor(2, 0);
  lcd.print("MDPRA IFSC"); // Tela de splash
  lcd.setCursor(2, 1);
  lcd.print("Bem-vindo!");
  delay(1600);

  pinMode(MOTOR_EN_PIN, OUTPUT); // Define pinos do motor
  pinMode(MOTOR_STEP_PIN, OUTPUT);
  pinMode(MOTOR_DIR_PIN, OUTPUT);
  pinMode(COOLER_PIN, OUTPUT); // Define pino do cooler
digitalWrite(MOTOR_EN_PIN, LOW); // Liga o motor (para travar o eixo)
  delay(50);

  if (!checkPCF()) { // Verifica o teclado
    lcd.clear();
    lcd.print("!!Erro Teclado!!");
    lcd.setCursor(0, 1);
    lcd.print("!Nao Detectado!");
    delay(1800);
    while (true); // Trava o código se falhar
} else {
    writePCF(0xFF); // Garante que todas as linhas do PCF estão em HIGH
  }

  if (!inicializarSD()) { // Verifica o cartão SD
    while (true); // Trava o código se falhar
  }

  sensors.begin(); // Inicia o sensor de temperatura
if (!as5600Begin()) { // Verifica o encoder AS5600
    lcd.clear();
    lcd.print("!!Erro AS5600!!");
    lcd.setCursor(0, 1);
    lcd.print("!Nao Detectado!");
    delay(1800);
    while(true); // Trava o código se falhar
  }

  preferences.begin("scanConfig", false); // Abre a NVS (R/W)
anguloTotal = preferences.getFloat("anguloTotal", 180.0); // Carrega Amplitude (ou 180.0)
  anguloPasso = preferences.getFloat("anguloPasso", 5.0); // Carrega Passo (ou 5.0)
  leiturasPA = preferences.getInt("leiturasPA", 1); // Carrega Leituras (ou 1)
  zeroPonto = preferences.getFloat("zeroPonto", 0.0); // Carrega Ponto Zero
  scanNumeroAtual = preferences.getInt("scanNumero", 0); // Carrega Contador (ou 0)
rfFilterValue = preferences.getFloat("rfFilter", 0.0); // Carrega Limiar RF
  buzzerMuted = preferences.getBool("buzzerMuted", false); // Carrega Mudo
  diaAtual = preferences.getInt("diaAtual", 11); // Carrega data simulada
  mesAtual = preferences.getInt("mesAtual", 9);
  anoAtual = preferences.getInt("anoAtual", 2025);
getAnguloFiltrado(); // Inicializa o filtro de ângulo

  lcd.clear();
  lcd.print("WiFi Conectando!");
  lcd.setCursor(0, 1);
  lcd.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS); // Tenta conectar ao WiFi
  unsigned long startTime = millis();
while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) { // Timeout de 20 segundos
    delay(500);
    lcd.print(".");
}
  if (WiFi.status() == WL_CONNECTED) { // Se conectou...
    configTime(-10800, 0, "pool.ntp.org", "time.nist.gov"); // Configura NTP (Fuso -3h = -10800s)
    struct tm timeinfo;
if (getLocalTime(&timeinfo)) { // Se conseguiu pegar a hora NTP...
        diaAtual = timeinfo.tm_mday; // Atualiza a data interna
        mesAtual = timeinfo.tm_mon + 1;
anoAtual = timeinfo.tm_year + 1900;
        preferences.putInt("diaAtual", diaAtual); // Salva a data NTP na NVS
        preferences.putInt("mesAtual", mesAtual);
        preferences.putInt("anoAtual", anoAtual);
    }

    lcd.clear();
    lcd.print("!WiFi Conectado!");
    lcd.setCursor(0, 1);
lcd.print(WiFi.localIP()); // Mostra o IP
    delay(1500);
    wifiConectado = true; // Seta a flag de conexão
  } else { // Se falhou...
    lcd.clear();
    lcd.print("!!WiFi Falhou!!");
    lcd.setCursor(0, 1);
    lcd.print("!!Desconectado!!");
    delay(1500);
wifiConectado = false; // Flag de conexão falsa
  }

  mostrarMenuPrincipal(); // Chama o menu principal (que é um loop infinito)
}

void loop() {
  if (millis() - ultimaVerificacaoTemp >= intervaloVerificacaoTemp) { // A cada 1 minuto...
    verificarTemperatura(); // ...verifica a temperatura
}
  atualizarDataSimulada(); // Atualiza a data (NTP ou simulada)
  if (scanAtivo) { // Se a flag `scanAtivo` for true...
    executarScan(); // ...executa a máquina de estados do scan
  } else { // Se a flag for false...
    mostrarMenuPrincipal(); // ...executa o loop do menu principal
  }
  delay(10); // Pequena pausa
}
