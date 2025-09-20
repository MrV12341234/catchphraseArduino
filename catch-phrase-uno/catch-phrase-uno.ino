/*
  Catchphrase (No Categories)
  Hardware:
    - Arduino Uno
    - 1602 LCD (parallel) on pins RS=8, E=9, D4=A0(14), D5=A1(15), D6=A2(16), D7=A3(17)
    - Backlight control on A4 (18). 220 ohm resiter between this pin (A4) and LCD pin A
    - SD TF card reader (CS=10, MOSI=11, MISO=12, SCK=13)
    - piezo Buzzer on D7
    - Buttons:
        START/STOP = D2
        TEAM1      = D3
        TEAM2      = D4
        NEXT       = D5
        CATEGORY   = D6  (repurposed as MUTE toggle)
*/

//text file (notepad) must be called words.txt
//12 characters per LCD line. The formatter tries to split on a space so the clue can use up to 2 lines.
//if a word is longer than 13 characters it gets skipped (no words longer than 13 characters)
// Blank lines or lines starting with # are ignored.

#include <SPI.h>
#include <SD.h>
#include <LiquidCrystal.h>

// ===== Pins (your mapping) =====
const byte TRANSISTOR_POWER_PIN = 19; // A5
const byte START_STOP_PIN = 2;
const byte TEAM1_PIN      = 3;
const byte TEAM2_PIN      = 4;
const byte NEXT_PIN       = 5;  // Next word
const byte CATEGORY_PIN   = 6;  // Mute toggle
const byte SPEAKER_PIN    = 7;

const byte LCD_PIN_RS = 8;
const byte LCD_PIN_E  = 9;
const byte SD_PIN_CS  = 10;

const byte LCD_PIN_D4 = 14; // A0
const byte LCD_PIN_D5 = 15; // A1
const byte LCD_PIN_D6 = 16; // A2
const byte LCD_PIN_D7 = 17; // A3
const byte LCD_PIN_BL = 18; // A4

// ===== LCD + SD =====
LiquidCrystal lcd(LCD_PIN_RS, LCD_PIN_E, LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7);
File wordsFile;

// ===== Scores (global so showScoresAndText sees them) =====
int score_team1 = 0;
int score_team2 = 0;

// ===== Display formatting =====
// Top row: [scoreL][ 14-char text ][scoreR]  => total 16 columns
// Bottom row: full 16-char window
#define TOP_TEXT_LEN     14
#define BOTTOM_TEXT_LEN  16

String pad_center(String text, uint8_t width) {
  text.trim();
  if (text.length() > width) return "";   // too long for this line
  uint8_t leftPad  = (width - text.length()) / 2;
  uint8_t rightPad = width - text.length() - leftPad;
  String s;
  for (uint8_t i=0;i<leftPad;i++)  s += ' ';
  s += text;
  for (uint8_t i=0;i<rightPad;i++) s += ' ';
  return s;
}

// Format a clue to fit: TOP=14 chars, BOTTOM=16 chars.
// Returns a single String with length TOP_TEXT_LEN + BOTTOM_TEXT_LEN.
// First 14 chars -> top text window; last 16 chars -> bottom line.
// splits a phrase at a space if total phrase is longer than the top_text length (14).
// if single word is longer than 14 letters, it is displayed on bottom line and top line is left empty (except the scores)
String format_for_lcd(String text) {
  text.trim();

  auto pad_center = [](const String &t, uint8_t width) {
    String s = t; s.trim();
    if (s.length() > width) return String(""); // fail
    uint8_t left  = (width - s.length()) / 2;
    uint8_t right = width - s.length() - left;
    String out;
    for (uint8_t i=0;i<left;i++)  out += ' ';
    out += s;
    for (uint8_t i=0;i<right;i++) out += ' ';
    return out;
  };

  if (text.length() == 0) {
    return pad_center("", TOP_TEXT_LEN) + pad_center("", BOTTOM_TEXT_LEN);
  }

  // ---- NEW: single long token (no spaces) of 15–16 chars -> show on bottom line ----
  if (text.indexOf(' ') < 0 && text.length() > TOP_TEXT_LEN && text.length() <= BOTTOM_TEXT_LEN) {
    String top    = pad_center("",   TOP_TEXT_LEN);     // blank top window
    String bottom = pad_center(text, BOTTOM_TEXT_LEN);  // word centered on full bottom row
    return (bottom.length() == 0) ? String("") : (top + bottom);
  }

  // Fits entirely on top?
  if (text.length() <= TOP_TEXT_LEN) {
    String top    = pad_center(text, TOP_TEXT_LEN);
    String bottom = pad_center("",   BOTTOM_TEXT_LEN);
    return (top.length() == 0 || bottom.length() == 0) ? String("") : (top + bottom);
  }

  // Two-line split at last space that keeps top within 14
  int lastSpaceTop = text.lastIndexOf(' ', TOP_TEXT_LEN);
  if (lastSpaceTop < 0) {
    // First token exceeds 14 and has no spaces -> can't split (and >16 handled above)
    return String("");
  }

  String topPart = text.substring(0, lastSpaceTop);
  String botPart = text.substring(lastSpaceTop + 1);
  botPart.trim();

  if (botPart.length() > BOTTOM_TEXT_LEN) {
    return String(""); // bottom would overflow
  }

  String top    = pad_center(topPart, TOP_TEXT_LEN);
  String bottom = pad_center(botPart, BOTTOM_TEXT_LEN);
  return (top.length() == 0 || bottom.length() == 0) ? String("") : (top + bottom);
}

void lcdClearLine(byte row) {
  lcd.setCursor(0,row);
  for (byte i=0;i<16;i++) lcd.print(' ');
}

void showScoresAndText(const String &mainText) {
  // mainText comes from format_for_lcd(): first 14 chars top-window, last 16 bottom-line
  String topWin = mainText.substring(0, TOP_TEXT_LEN);
  String bot    = mainText.substring(TOP_TEXT_LEN); // 16 chars

  // Top: put left score at col0, right score at col15, and the 14-char window in between
  lcd.setCursor(0,0);  lcd.print(score_team1);   // col 0
  lcd.setCursor(1,0);  lcd.print(topWin);        // cols 1..14
  lcd.setCursor(15,0); lcd.print(score_team2);   // col 15

  // Bottom: draw the full 16-char centered line
  lcd.setCursor(0,1);
  lcd.print(bot);
}

// ===== Debounced buttons =====
struct DebouncedButton {
  byte pin;
  byte lastAdvertised;
  byte curAdvertised;
  byte lastRead;
  unsigned long lastChange;
  void begin(byte p) { pin=p; pinMode(p, INPUT_PULLUP); lastAdvertised=curAdvertised=lastRead=HIGH; lastChange=0; }
  void update() {
    byte s = digitalRead(pin);
    unsigned long now = millis();
    if (s != lastRead) lastChange = now;
    if (now - lastChange > 50) curAdvertised = s;
    lastRead = s;
  }
  bool justPressed()  { bool jp = (curAdvertised != lastAdvertised) && (curAdvertised == LOW);  lastAdvertised = curAdvertised; return jp; }
  bool justReleased() { bool jr = (curAdvertised != lastAdvertised) && (curAdvertised == HIGH); lastAdvertised = curAdvertised; return jr; }
  bool isPressed()    { return curAdvertised == LOW; }
};

DebouncedButton btnStart, btnT1, btnT2, btnNext, btnMute;

// ===== Game state =====
enum GAME_STATE { READY, IN_ROUND, GAME_DONE };
GAME_STATE gameState = READY;

bool muted = false;
String currentWord;

// ===== Beep timing (speeds up) =====
unsigned long beep_frequency_change_interval_millis = 15000;
unsigned long beep_interval_millis[] = {500, 500, 300, 200};
const int NUM_BEEP_INTERVALS = 4;

int cur_beep_interval = 0;
bool next_is_tic = true;
unsigned long last_tictoc_millis = 0;
unsigned long last_beep_speed_change_millis = 0;

// ===== Random word index (offset table, shuffled in-place) =====
// Keep memory small for Uno
const uint16_t MAX_WORDS = 120;               // ~480 bytes (120 * 4B) — SAFE for Uno
unsigned long wordOffsets[MAX_WORDS];
uint16_t wordCount = 0;
uint16_t wordPos = 0;
bool rng_seeded = false;

void fisherYatesShuffleOffsets() {
  if (wordCount <= 1) return;
  for (int i = wordCount - 1; i > 0; --i) {
    int j = random(i + 1); // 0..i
    unsigned long tmp = wordOffsets[i];
    wordOffsets[i] = wordOffsets[j];
    wordOffsets[j] = tmp;
  }
}

bool indexWords() {
  wordCount = 0;
  wordsFile.seek(0);
  while (true) {
    unsigned long startPos = wordsFile.position();
    String line = wordsFile.readStringUntil('\n');
    if (line.length() == 0 && !wordsFile.available()) break; // EOF

    String copy = line; copy.trim();
    if (copy.length() == 0) continue;            // skip blanks
    if (copy.startsWith("#")) continue;          // skip comments
    if (format_for_lcd(copy).length() == 0) continue; // too long to display nicely

    if (wordCount < MAX_WORDS) {
      wordOffsets[wordCount++] = startPos;
    } else {
      // hit cap — ignore remainder to save RAM
      break;
    }
  }
  wordPos = 0;
  fisherYatesShuffleOffsets();
  return wordCount > 0;
}

bool getNextRandomWord(String &out) {
  if (wordCount == 0) return false;
  if (wordPos >= wordCount) {
    fisherYatesShuffleOffsets();
    wordPos = 0;
  }
  unsigned long off = wordOffsets[wordPos++];
  wordsFile.seek(off);
  String line = wordsFile.readStringUntil('\n');
  line.trim();
  out = line;
  return out.length() > 0;
}

// ===== Beeps =====
void beep_tic()      { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_toc()      { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_times_up() {
  if (!muted) { tone(SPEAKER_PIN, 300, 300); delay(300); tone(SPEAKER_PIN, 300, 300); delay(300); tone(SPEAKER_PIN, 300, 300); }
  else { delay(900); }
}
void beep_power_on() { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_small()    { if (!muted) tone(SPEAKER_PIN, 300, 30); }

// ===== Round flow =====
void showWord(const String &word) {
  String two = format_for_lcd(word);
  if (two.length() == 0) two = format_for_lcd("(too long)");
  showScoresAndText(two);
}

void startRound() {
  gameState = IN_ROUND;
  cur_beep_interval = 0;
  next_is_tic = true;
  last_tictoc_millis = 0;
  last_beep_speed_change_millis = millis();

  if (!getNextRandomWord(currentWord)) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print(F("No words indexed"));
    lcd.setCursor(0,1); lcd.print(F("Check words.txt"));
    return;
  }
  showWord(currentWord);
}

void endRound(bool timesUp=true) {
  if (timesUp) beep_times_up();
  gameState = READY;
  String two = format_for_lcd("Press Start");
  showScoresAndText(two);
}

void do_tic_toc() {
  unsigned long now = millis();
  if (now - last_beep_speed_change_millis > beep_frequency_change_interval_millis) {
    last_beep_speed_change_millis = now;
    if (++cur_beep_interval >= NUM_BEEP_INTERVALS) { endRound(true); return; }
  }
  if (now - last_tictoc_millis > beep_interval_millis[cur_beep_interval]) {
    if (next_is_tic) beep_tic(); else beep_toc();
    next_is_tic = !next_is_tic;
    last_tictoc_millis = now;
  }
}

// ===== Setup / Loop =====
void setup() {
  pinMode(TRANSISTOR_POWER_PIN, OUTPUT);
  digitalWrite(TRANSISTOR_POWER_PIN, HIGH);

  pinMode(LCD_PIN_BL, OUTPUT);
  digitalWrite(LCD_PIN_BL, HIGH);

  pinMode(SPEAKER_PIN, OUTPUT);

  btnStart.begin(START_STOP_PIN);
  btnT1.begin(TEAM1_PIN);
  btnT2.begin(TEAM2_PIN);
  btnNext.begin(NEXT_PIN);
  btnMute.begin(CATEGORY_PIN);

  lcd.begin(16,2);
  lcdClearLine(0); lcdClearLine(1);
  lcd.setCursor(0,0); lcd.print(F("Loading SD..."));

  pinMode(SD_PIN_CS, OUTPUT);
  digitalWrite(SD_PIN_CS, HIGH);
  if (!SD.begin(SD_PIN_CS)) {
    lcd.setCursor(0,1); lcd.print(F("SD FAIL"));
    while (1) { }
  }

  wordsFile = SD.open("words.txt", FILE_READ);
  if (!wordsFile) wordsFile = SD.open("WORDS.TXT", FILE_READ);
  if (!wordsFile) {
    lcd.setCursor(0,1); lcd.print(F("words.txt?"));
    while (1) { }
  }

  // Index and shuffle
  if (!indexWords()) {
    lcd.setCursor(0,1); lcd.print(F("No words found"));
    while (1) { }
  }

  // Seed RNG on boot (extra entropy later on first button)
  randomSeed(analogRead(A5)); // floating analog adds some noise

  beep_power_on();
  String two = format_for_lcd("Press Start");
  showScoresAndText(two);
  gameState = READY;
  score_team1 = 0; score_team2 = 0;
}

void loop() {
  btnStart.update();
  btnT1.update();
  btnT2.update();
  btnNext.update();
  btnMute.update();

  if (!rng_seeded && (btnStart.justPressed() || btnT1.justPressed() || btnT2.justPressed() || btnNext.justPressed() || btnMute.justPressed())) {
    randomSeed(micros());
    rng_seeded = true;
  }

  if (btnMute.justPressed()) {
    muted = !muted;
    lcdClearLine(1);
    lcd.setCursor(4,1);
    lcd.print(muted ? F("Muted") : F("Sound On"));
    if (!muted) beep_small();
    delay(300);
    if (gameState == IN_ROUND) showWord(currentWord);
    else {
      String two = format_for_lcd("Press Start");
      showScoresAndText(two);
    }
  }

  switch (gameState) {
    case READY:
      if (btnStart.justPressed()) startRound();
      if (btnT1.justPressed()) {
        score_team1++; beep_small();
        if (score_team1 == 7) { lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Team1 Wins!")); gameState = GAME_DONE; }
        else { String two = format_for_lcd("Press Start");
        showScoresAndText(two); }
      }
      if (btnT2.justPressed()) {
        score_team2++; beep_small();
        if (score_team2 == 7) { lcd.clear(); lcd.setCursor(0,0); lcd.print(F("Team2 Wins!")); gameState = GAME_DONE; }
        else { String two = format_for_lcd("Press Start"); showScoresAndText(two); }
      }
      break;

    case IN_ROUND:
      if (btnStart.justPressed()) { endRound(false); break; } // stop early
      if (btnNext.justPressed()) {
        if (getNextRandomWord(currentWord)) showWord(currentWord);
      }
      do_tic_toc();
      break;

    case GAME_DONE:
      if (btnStart.justPressed()) {
        score_team1 = 0; score_team2 = 0;
        String two = format_for_lcd("Press Start");
        showScoresAndText(two);
        gameState = READY;
      }
      break;
  }

  delay(5);
}
