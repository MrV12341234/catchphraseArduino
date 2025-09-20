/*
  Catchphrase (No Categories) — Simple + Stable
  Hardware:
    - Arduino Uno
    - 1602 LCD (parallel) on pins RS=8, E=9, D4=A0(14), D5=A1(15), D6=A2(16), D7=A3(17)
      Backlight control on A4 (18)
    - SD TF card reader (CS=10, MOSI=11, MISO=12, SCK=13)
    - Buzzer on D7
    - Buttons:
        START/STOP = D2
        TEAM1      = D3
        TEAM2      = D4
        NEXT       = D5
        CATEGORY   = D6  (repurposed as MUTE toggle)
*/

#include <SPI.h>
#include <SD.h>
#include <LiquidCrystal.h>

// ===== Pins (your mapping) =====
const byte TRANSISTOR_POWER_PIN = 19; // A5 (optional)
const byte START_STOP_PIN = 2;
const byte TEAM1_PIN      = 3;
const byte TEAM2_PIN      = 4;
const byte NEXT_PIN       = 5;
const byte CATEGORY_PIN   = 6;  // mute toggle
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

// ===== Scores must be visible to all functions that print =====
int score_team1 = 0;
int score_team2 = 0;

// ===== Display formatting =====
#define MAX_DISPLAY_LINE_LENGTH 12

String pad_display_line(String text) {
  byte leftPad  = (MAX_DISPLAY_LINE_LENGTH - text.length()) / 2;
  byte rightPad = MAX_DISPLAY_LINE_LENGTH - text.length() - leftPad;
  String s = "";
  for (byte i=0;i<leftPad;i++)  s += ' ';
  s += text;
  for (byte i=0;i<rightPad;i++) s += ' ';
  return s;
}

// Centers text across two 12-char halves (top/bottom). Returns "" if it can’t fit.
String format_for_lcd(String text) {
  text.trim();
  if (text.length() == 0) return "";

  short splitPos = -1;
  while (text.length() > MAX_DISPLAY_LINE_LENGTH) {
    short nextSpace = text.indexOf(' ', splitPos + 1);
    if (nextSpace == -1 || nextSpace > MAX_DISPLAY_LINE_LENGTH) break;
    splitPos = nextSpace;
  }

  String top, bottom;
  if (splitPos == -1) { top = text; bottom = ""; }
  else { top = text.substring(0, splitPos); bottom = text.substring(splitPos + 1); }

  if (top.length() > MAX_DISPLAY_LINE_LENGTH || bottom.length() > MAX_DISPLAY_LINE_LENGTH) return "";

  return pad_display_line(top) + pad_display_line(bottom);
}

void lcdClearLine(byte row) {
  lcd.setCursor(0,row);
  for (byte i=0;i<16;i++) lcd.print(' ');
}

void showScoresAndText(String mainText) {
  // mainText is expected to be 24 chars (12+12) from format_for_lcd
  String top = mainText.substring(0, 12);
  String bot = mainText.substring(12);
  // Top row: <score1> SPACE <12 chars> SPACE <score2>
  lcd.setCursor(0,0);
  lcd.print(score_team1);
  lcd.print(' ');
  lcd.print(top);
  lcd.print(' ');
  lcd.print(score_team2);
  // Bottom row: fully clear then print
  lcdClearLine(1);
  lcd.setCursor(2,1);  // gutter like your original
  lcd.print(bot);
}

// ===== Button helper =====
struct DebouncedButton {
  byte pin;
  byte lastAdvertised = HIGH;
  byte curAdvertised  = HIGH;
  byte lastRead       = HIGH;
  unsigned long lastChange = 0;

  void begin(byte p) { pin=p; pinMode(p, INPUT_PULLUP); lastAdvertised=curAdvertised=lastRead=HIGH; lastChange=0; }
  void update() {
    byte s = digitalRead(pin);
    unsigned long now = millis();
    if (s != lastRead) lastChange = now;
    if (now - lastChange > 50) curAdvertised = s;
    lastRead = s;
  }
  bool isPressed() { return curAdvertised == LOW; }
  bool justPressed() { bool jp = (curAdvertised != lastAdvertised) && (curAdvertised == LOW); lastAdvertised = curAdvertised; return jp; }
  bool justReleased(){ bool jr = (curAdvertised != lastAdvertised) && (curAdvertised == HIGH); lastAdvertised = curAdvertised; return jr; }
};

DebouncedButton btnStart, btnT1, btnT2, btnNext, btnMute;

// ===== Game state =====
enum GAME_STATE { READY, IN_ROUND, GAME_DONE };
GAME_STATE gameState = READY;

bool muted = false;
String currentWord = "";

// ===== Beep timing (speeds up like your original) =====
unsigned long beep_frequency_change_interval_millis = 15000;        // speed-up period
unsigned long beep_interval_millis[] = {500, 500, 300, 200};        // tic/toc gap per phase
const int NUM_BEEP_INTERVALS = 4;

int cur_beep_interval = 0;
bool next_is_tic = true;
unsigned long last_tictoc_millis = 0;
unsigned long last_beep_speed_change_millis = 0;

// ===== Helpers =====
void beep_tic() { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_toc() { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_times_up() {
  if (!muted) {
    tone(SPEAKER_PIN, 300, 300); delay(300);
    tone(SPEAKER_PIN, 300, 300); delay(300);
    tone(SPEAKER_PIN, 300, 300);
  } else {
    delay(900);
  }
}
void beep_power_on() { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_small()    { if (!muted) tone(SPEAKER_PIN, 300, 30); }

// Read next non-empty, non-comment line; if EOF, rewind and continue.
bool readNextWord(String &out) {
  while (true) {
    if (!wordsFile) return false;
    String line = wordsFile.readStringUntil('\n');
    if (!wordsFile.available() && line.length()==0) {
      // EOF with no data read — rewind and try one more
      wordsFile.seek(0);
      line = wordsFile.readStringUntil('\n');
    }
    if (line.length()==0 && !wordsFile.available()) return false;

    line.trim();
    if (line.length()==0) continue;           // skip blank
    if (line.startsWith("#")) continue;       // skip comment

    String twoLine = format_for_lcd(line);
    if (twoLine.length() == 0) continue;      // doesn’t fit nicely — try next

    out = line;
    return true;
  }
}

void showWord(String word) {
  String two = format_for_lcd(word);
  if (two.length() == 0) two = pad_display_line("(too long)") + pad_display_line("");
  showScoresAndText(two);
}

void startRound() {
  gameState = IN_ROUND;
  cur_beep_interval = 0;
  next_is_tic = true;
  last_tictoc_millis = 0;
  last_beep_speed_change_millis = millis();

  if (!readNextWord(currentWord)) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("No words file");
    lcd.setCursor(0,1); lcd.print("or empty!");
    return;
  }
  showWord(currentWord);
}

void endRound(bool timesUp=true) {
  if (timesUp) beep_times_up();
  gameState = READY;
  String two = pad_display_line("Press Start") + pad_display_line("");
  showScoresAndText(two);
}

void do_tic_toc() {
  unsigned long now = millis();
  if (now - last_beep_speed_change_millis > beep_frequency_change_interval_millis) {
    last_beep_speed_change_millis = now;
    if (++cur_beep_interval >= NUM_BEEP_INTERVALS) {
      endRound(true);
      return;
    }
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
  lcd.setCursor(0,0); lcd.print("Init SD...");

  pinMode(SD_PIN_CS, OUTPUT);
  digitalWrite(SD_PIN_CS, HIGH);
  if (!SD.begin(SD_PIN_CS)) {
    lcd.setCursor(0,1); lcd.print("SD FAIL");
    while (1) { /* halt */ }
  }

  wordsFile = SD.open("words.txt", FILE_READ);
  if (!wordsFile) wordsFile = SD.open("WORDS.TXT", FILE_READ);
  if (!wordsFile) {
    lcd.setCursor(0,1); lcd.print("words.txt?");
    while (1) { /* halt */ }
  }

  beep_power_on();
  String two = pad_display_line("Press Start") + pad_display_line("");
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

  // Mute toggle on CATEGORY button
  if (btnMute.justPressed()) {
    muted = !muted;
    lcdClearLine(1);
    lcd.setCursor(4,1);
    lcd.print(muted ? "Muted" : "Sound On");
    if (!muted) beep_small();
    delay(300);
    if (gameState == IN_ROUND) showWord(currentWord);
    else {
      String two = pad_display_line("Press Start") + pad_display_line("");
      showScoresAndText(two);
    }
  }

  switch (gameState) {
    case READY:
      if (btnStart.justPressed()) startRound();

      if (btnT1.justPressed()) {
        score_team1++; beep_small();
        if (score_team1 == 7) { lcd.clear(); lcd.setCursor(0,0); lcd.print("Team1 Wins!"); gameState = GAME_DONE; }
        else { String two = pad_display_line("Press Start") + pad_display_line(""); showScoresAndText(two); }
      }
      if (btnT2.justPressed()) {
        score_team2++; beep_small();
        if (score_team2 == 7) { lcd.clear(); lcd.setCursor(0,0); lcd.print("Team2 Wins!"); gameState = GAME_DONE; }
        else { String two = pad_display_line("Press Start") + pad_display_line(""); showScoresAndText(two); }
      }
      break;

    case IN_ROUND:
      if (btnStart.justPressed()) { endRound(false); break; } // stop early
      if (btnNext.justPressed()) {
        if (readNextWord(currentWord)) showWord(currentWord);
      }
      do_tic_toc();
      break;

    case GAME_DONE:
      if (btnStart.justPressed()) {
        score_team1 = 0; score_team2 = 0;
        String two = pad_display_line("Press Start") + pad_display_line("");
        showScoresAndText(two);
        gameState = READY;
      }
      break;
  }

  delay(5);
}
