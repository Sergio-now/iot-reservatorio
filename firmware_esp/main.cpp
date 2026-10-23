#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>

// --- Configurações de Rede e Broker MQTT ---
const char* ssid = "SUA_REDE_WIFI";
const char* password = "SUA_SENHA_WIFI";
const char* mqtt_server = "192.168.1.XX"; // IP da máquina onde o HiveMQ está rodando

WiFiClient espClient;
PubSubClient client(espClient);
HardwareSerial SensorSerial(1);

// --- Pinagem UART para o Sensor Ultrassônico (JSN-SR04T / SR04M-2) ---
const int RX_PIN = 13; // Conectar ao pino TX do sensor
const int TX_PIN = 17; // Conectar ao pino RX do sensor

// --- Parâmetros de Filtro e Buffer Circular ---
const int numReadings = 15;      
float readings[numReadings];     
int readIndex = 0;               
bool primed = false;             

unsigned char buffer_RTT[4] = {0}; 
unsigned char CS; 

unsigned long lastPub = 0;
unsigned long lastReconnect = 0;
unsigned long lastValidFrame = 0;

void setup() {
  Serial.begin(115200);
  SensorSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
  }
  Serial.println("\nWiFi Conectado!");

  client.setServer(mqtt_server, 1883);
  client.setSocketTimeout(2); 
}

void reconnectMqtt() {
  if (millis() - lastReconnect > 5000) {
    lastReconnect = millis();
   
    // Conexão com Last Will and Testament (LWT)
    // Se o ESP32 desconectar abruptamente, o broker publica "offline" automaticamente
    if (client.connect("Heltec_ESP32", "sensores/nivel_agua/status", 0, true, "offline")) {
      Serial.println("Conectado ao Broker HiveMQ!");
      client.publish("sensores/nivel_agua/status", "online", true);
    }
  }
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  } else {
    client.loop();
  }

  // Falha silenciosa do sensor (cabo solto ou erro no SR04M-2)
  if (millis() - lastValidFrame > 10000 && millis() - lastPub >= 3000) {
    lastPub = millis();
    client.publish("sensores/nivel_agua/status", "sem_leitura", true);
  }

  if (SensorSerial.available() >= 4) {
    if (SensorSerial.read() == 0xFF) {
      buffer_RTT[0] = 0xFF;
      for (int i = 1; i < 4; i++) buffer_RTT[i] = SensorSerial.read();
     
      CS = buffer_RTT[0] + buffer_RTT[1] + buffer_RTT[2];  
      if (buffer_RTT[3] == CS) {
        float rawDistance = ((buffer_RTT[1] << 8) + buffer_RTT[2]) / 10.0;
        
        // Linha temporária para depuração local
        Serial.printf("Raw: %.1f cm\n", rawDistance); 
        
        // Filtro de segurança (Zona Cega e Ruído fora da faixa linear)
        if (rawDistance < 20.0 || rawDistance > 55.0) return;

        lastValidFrame = millis(); 

        if (!primed) {
          for (int i = 0; i < numReadings; i++) readings[i] = rawDistance;
          primed = true;
        }
       
        readings[readIndex] = rawDistance;
        readIndex = (readIndex + 1) % numReadings;
        float tempReadings[numReadings];
        for (int i = 0; i < numReadings; i++) tempReadings[i] = readings[i];
       
        // Ordenação das leituras para aplicação da Média Aparada
        for (int i = 0; i < numReadings - 1; i++) {
          for (int j = i + 1; j < numReadings; j++) {
            if (tempReadings[i] > tempReadings[j]) {
              float swap = tempReadings[i];
              tempReadings[i] = tempReadings[j];
              tempReadings[j] = swap;
            }
          }
        }
       
        // Descarte dos 3 menores e 3 maiores valores (corte de caudas)
        float soma = 0;
        for (int i = 3; i < numReadings - 3; i++) soma += tempReadings[i];
        float mediaAparada_cm = soma / (numReadings - 6);
       
        // Conversão com a curva de calibração volumétrica
        float volumeReal_mL = (mediaAparada_cm - 47.201) / -0.02074;
        if (volumeReal_mL < 0) volumeReal_mL = 0;

        if (millis() - lastPub >= 3000) {
          lastPub = millis();
          String payload = "{\"distancia_cm\": " + String(mediaAparada_cm, 1) + 
                           ", \"volume_ml\": " + String(volumeReal_mL, 1) + "}";
         
          client.publish("sensores/nivel_agua", payload.c_str(), true);
          client.publish("sensores/nivel_agua/status", "online", true);
         
          // Impressão no Monitor Serial para acompanhamento
          Serial.printf("Distância: %.1f cm | Volume: %.1f mL\n", mediaAparada_cm, volumeReal_mL);
        }
      }
    }
  } 
}