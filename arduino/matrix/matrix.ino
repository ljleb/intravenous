const int sourcePins[] = {2, 3, 4, 5, 6, 7};
const int inputPins[]  = {A0, A1, A2, A3, A4, A5};

const uint8_t sourceCount = sizeof(sourcePins) / sizeof(sourcePins[0]);
const uint8_t inputCount = sizeof(inputPins) / sizeof(inputPins[0]);
const uint8_t maxEvents = sourceCount * inputCount;

const uint8_t FRAME_H0 = 0xFF;
const uint8_t FRAME_H1 = 0xFF;

int threshold = 40;
int settleMicros = 20;

uint8_t sequence = 0;

char commandBuffer[32];
uint8_t commandLength = 0;

void allSourcesHighZ() {
  for (uint8_t s = 0; s < sourceCount; s++) {
    pinMode(sourcePins[s], INPUT);
  }
}

uint16_t packEvent(uint8_t id, uint16_t value) {
  return ((uint16_t)id << 10) | (value & 0x03FF);
}

void sendFrame(uint16_t* events, uint8_t count) {
  uint8_t checksum = 0;

  Serial.write(FRAME_H0);
  Serial.write(FRAME_H1);

  Serial.write(sequence);
  checksum ^= sequence;

  Serial.write(count);
  checksum ^= count;

  for (uint8_t i = 0; i < count; i++) {
    uint8_t lo = events[i] & 0xFF;
    uint8_t hi = events[i] >> 8;

    Serial.write(lo);
    Serial.write(hi);

    checksum ^= lo;
    checksum ^= hi;
  }

  Serial.write(checksum);

  sequence++;
}

void scanAndSendActiveSnapshot() {
  uint16_t events[maxEvents];
  uint8_t eventCount = 0;

  for (uint8_t s = 0; s < sourceCount; s++) {
    allSourcesHighZ();

    pinMode(sourcePins[s], OUTPUT);
    digitalWrite(sourcePins[s], HIGH);

    delayMicroseconds(settleMicros);

    for (uint8_t d = 0; d < inputCount; d++) {
      int raw = analogRead(inputPins[d]);

      if (raw >= threshold) {
        uint8_t id = s * inputCount + d;
        uint16_t value = constrain(raw, 0, 1023);

        events[eventCount++] = packEvent(id, value);
      }
    }
  }

  allSourcesHighZ();

  // Send even if eventCount == 0.
  // Empty frame means: all connections are currently inactive.
  sendFrame(events, eventCount);
}

void sendStatus() {
  // Human-readable config response.
  // Your binary parser should ignore bytes until it sees 0xFF 0xFF.
  Serial.print("STATUS THRESH ");
  Serial.print(threshold);
  Serial.print(" SETTLE ");
  Serial.print(settleMicros);
  Serial.print(" BAUD ");
  Serial.println(2000000);
}

void handleCommand(char* cmd) {
  // Commands are ASCII lines:
  //
  //   T 100\n   set threshold to 100
  //   S 20\n    set settle delay to 20 microseconds
  //   STATUS\n  print current settings
  //
  // The Arduino keeps streaming binary frames regardless.

  while (*cmd == ' ') {
    cmd++;
  }

  if (cmd[0] == 'T' || cmd[0] == 't') {
    int v = atoi(cmd + 1);

    if (v >= 0 && v <= 1023) {
      threshold = v;
    }

    return;
  }

  if (cmd[0] == 'S' || cmd[0] == 's') {
    // Avoid treating STATUS as SETTLE.
    if (
      cmd[1] == 'T' || cmd[1] == 't' ||
      cmd[1] == 'A' || cmd[1] == 'a'
    ) {
      sendStatus();
      return;
    }

    int v = atoi(cmd + 1);

    if (v >= 0 && v <= 10000) {
      settleMicros = v;
    }

    return;
  }

  if (
    strcmp(cmd, "STATUS") == 0 ||
    strcmp(cmd, "status") == 0
  ) {
    sendStatus();
    return;
  }
}

void readCommands() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (commandLength > 0) {
        commandBuffer[commandLength] = '\0';
        handleCommand(commandBuffer);
        commandLength = 0;
      }
    } else {
      if (commandLength < sizeof(commandBuffer) - 1) {
        commandBuffer[commandLength++] = c;
      } else {
        // Overflow: discard bad command.
        commandLength = 0;
      }
    }
  }
}

void setup() {
  Serial.begin(2000000);

  // Faster ADC than Arduino default.
  // Default prescaler is 128.
  // This uses prescaler 16.
  // It is noisier, but much faster and suitable for this patch controller.
  ADCSRA = (ADCSRA & 0b11111000) | 0x04;

  allSourcesHighZ();
}

void loop() {
  readCommands();
  scanAndSendActiveSnapshot();
}