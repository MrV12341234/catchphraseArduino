
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
//14 characters per LCD line. The formatter tries to split on a space so the clue can use up to 2 lines.
//if a word is longer than 13 characters it gets skipped (no words longer than 13 characters)
// Blank lines or lines starting with # are ignored.

// Format a clue to fit: TOP=14 chars, BOTTOM=16 chars.
// Returns a single String with length TOP_TEXT_LEN + BOTTOM_TEXT_LEN.
// First 14 chars -> top text window; last 16 chars -> bottom line.
// splits a phrase at a space if total phrase is longer than the top_text length (14).
// if single word is longer than 14 letters, but no longer than 16, it is displayed on bottom line and top line is left empty (except the scores)
// any words 17+ letters is skipped.
// this is my script that works great. Just limited to 120 words per sd card

/*
  Catchphrase (No Categories)
  - Keeps one 120-word deck across multiple rounds (no repeats within deck)
  - Builds the deck on FIRST Start press (not in setup) so the first batch is random each power-up
  - If words.txt has <=120 usable lines: uses all of them; reshuffles only after all shown once
  - If words.txt has >120 usable: random 120-sample; rebuilds a new random 120 only when deck is exhausted
*/

#include <SPI.h>
#include <SD.h>
#include <LiquidCrystal.h>

// ===== Pins =====
const byte TRANSISTOR_POWER_PIN = 19; // A5
const byte START_STOP_PIN = 2;
const byte TEAM1_PIN      = 3;
const byte TEAM2_PIN      = 4;
const byte NEXT_PIN       = 5;
const byte CATEGORY_PIN   = 6;  // Mute toggle
const byte SPEAKER_PIN    = 7;

const byte LCD_PIN_RS = 8;
const byte LCD_PIN_E  = 9;
const byte SD_PIN_CS  = 10;

const byte LCD_PIN_D4 = 14; // A0
const byte LCD_PIN_D5 = 15; // A1
const byte LCD_PIN_D6 = 16; // A2
const byte LCD_PIN_D7 = 17; // A3
const byte LCD_PIN_BL = 18; // A4 (backlight via 220Ω to LCD A)

// ===== LCD + SD =====
LiquidCrystal lcd(LCD_PIN_RS, LCD_PIN_E, LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7);
File wordsFile;

// ===== Scores =====
int score_team1 = 0;
int score_team2 = 0;

// ===== Display formatting =====
#define TOP_TEXT_LEN     14
#define BOTTOM_TEXT_LEN  16

String pad_center(String text, uint8_t width) {
  text.trim();
  if (text.length() > width) return "";
  uint8_t leftPad  = (width - text.length()) / 2;
  uint8_t rightPad = width - text.length() - leftPad;
  String s;
  for (uint8_t i=0;i<leftPad;i++)  s += ' ';
  s += text;
  for (uint8_t i=0;i<rightPad;i++) s += ' ';
  return s;
}

// ---- VALIDATOR (no padding)
bool canDisplayRaw(const String &raw) {
  String t = raw; t.trim();
  if (t.length() == 0)       return false;     // blank
  if (t.startsWith("#"))     return false;     // comment

  if (t.indexOf(' ') < 0) {
    return (t.length() <= BOTTOM_TEXT_LEN);    // single token ≤16 ok
  }

  if (t.length() <= TOP_TEXT_LEN) return true; // whole phrase fits top

  int cut = t.lastIndexOf(' ', TOP_TEXT_LEN);
  if (cut < 0) return false;                   // first token too long

  String bottom = t.substring(cut + 1);
  bottom.trim();
  return (bottom.length() <= BOTTOM_TEXT_LEN); // bottom must fit
}

// ---- FORMATTER (padding + centering)
String format_for_lcd(String text) {
  text.trim();
  auto pad = [](const String &t, uint8_t width) {
    String s = t; s.trim();
    if (s.length() > width) return String("");
    uint8_t L = (width - s.length()) / 2;
    uint8_t R = width - s.length() - L;
    String out; for (uint8_t i=0;i<L;i++) out+=' '; out+=s; for (uint8_t i=0;i<R;i++) out+=' ';
    return out;
  };

  if (text.length() == 0) return pad("", TOP_TEXT_LEN) + pad("", BOTTOM_TEXT_LEN);

  if (text.indexOf(' ') < 0 && text.length() > TOP_TEXT_LEN && text.length() <= BOTTOM_TEXT_LEN) {
    // single token 15–16 -> bottom line centered
    return pad("", TOP_TEXT_LEN) + pad(text, BOTTOM_TEXT_LEN);
  }

  if (text.length() <= TOP_TEXT_LEN) {
    return pad(text, TOP_TEXT_LEN) + pad("", BOTTOM_TEXT_LEN);
  }

  int lastSpaceTop = text.lastIndexOf(' ', TOP_TEXT_LEN);
  if (lastSpaceTop < 0) return String("");

  String topPart = text.substring(0, lastSpaceTop);
  String botPart = text.substring(lastSpaceTop + 1);
  botPart.trim();
  if (botPart.length() > BOTTOM_TEXT_LEN) return String("");

  return pad(topPart, TOP_TEXT_LEN) + pad(botPart, BOTTOM_TEXT_LEN);
}

void lcdClearLine(byte row) {
  lcd.setCursor(0,row);
  for (byte i=0;i<16;i++) lcd.print(' ');
}

void showScoresAndText(const String &mainText) {
  String topWin = mainText.substring(0, TOP_TEXT_LEN);
  String bot    = mainText.substring(TOP_TEXT_LEN);
  lcd.setCursor(0,0);  lcd.print(score_team1);
  lcd.setCursor(1,0);  lcd.print(topWin);
  lcd.setCursor(15,0); lcd.print(score_team2);
  lcd.setCursor(0,1);  lcd.print(bot);
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

// ===== Persistent deck (max 120) =====
const uint16_t MAX_WORDS = 120;
unsigned long wordOffsets[MAX_WORDS]; // deck of offsets
uint16_t wordCount = 0;               // size of current deck (<=120)
uint16_t wordPos   = 0;               // next index to serve
uint32_t displayableTotal = 0;        // total usable lines in file (computed when building deck)
bool deckBuilt = false;               // built yet?

void fisherYatesShuffleDeck() {
  if (wordCount <= 1) return;
  for (int i = wordCount - 1; i > 0; --i) {
    int j = random(i + 1);
    unsigned long tmp = wordOffsets[i];
    wordOffsets[i] = wordOffsets[j];
    wordOffsets[j] = tmp;
  }
}

// RNG seeding — use human timing jitter
void reseedRNG() {
  unsigned long t = micros() ^ (millis() << 16);
  randomSeed(t);
}

// Build a deck (once or when exhausted):
// - If total usable lines <=120: use ALL of them (wordCount = total), shuffled.
// - If >120: pick a random 120-sample via reservoir sampling.
// Returns false if no usable lines found.
bool buildDeckReservoir() {
  wordsFile.seek(0);
  displayableTotal = 0;
  wordCount = 0;

  while (true) {
    unsigned long startPos = wordsFile.position();
    String line = wordsFile.readStringUntil('\n');
    if (line.length() == 0 && !wordsFile.available()) break;

    String t = line; t.trim();
    if (t.length() == 0) continue;
    if (t.startsWith("#")) continue;
    if (!canDisplayRaw(t)) continue;

    displayableTotal++;
    if (wordCount < MAX_WORDS) {
      wordOffsets[wordCount++] = startPos;
    } else {
      // reservoir sampling
      uint32_t j = (uint32_t)random(displayableTotal); // 0..displayableTotal-1
      if (j < MAX_WORDS) wordOffsets[j] = startPos;
    }
  }

  if (displayableTotal == 0) return false;
  if (displayableTotal < MAX_WORDS) wordCount = (uint16_t)displayableTotal;

  fisherYatesShuffleDeck();
  wordPos = 0;
  deckBuilt = true;
  return true;
}

// function for displaying the word loading in the center. called in getNextWordFromDeck(). Shown both at the start of the game when the game is loading 
// the first batch. Also shown if in the middle of a round, the current batch of 120 words is exhausted.
void showLoading() {
  // show "Loading..." centered-ish on the bottom row
  lcd.setCursor(0,1);
  lcd.print("                ");  // clear bottom
  lcd.setCursor(3,1);            // rough center for 10 chars
  lcd.print("Loading...");
}


// Serve next word; if deck exhausted, rebuild per rules
bool getNextWordFromDeck(String &out) {
  // Need a deck for the first time OR we’ve exhausted the current deck
  // if current 120 word deck is exhausted during a round, pause timer & show the loading splash for the brief pause
  if (!deckBuilt || wordPos >= wordCount) {
    unsigned long pauseStart = millis();
    showLoading();

    if (!deckBuilt) {
      // First-time build
      reseedRNG();
      if (!buildDeckReservoir()) return false;
    } else if (displayableTotal <= MAX_WORDS) {
      // ≤120 usable total: reshuffle same full set after it’s all been shown
      fisherYatesShuffleDeck();
      wordPos = 0;
    } else {
      // >120 usable: build a fresh random 120-sample
      reseedRNG();
      if (!buildDeckReservoir()) return false;
    }

    // Don’t consume round time during the brief load
    unsigned long pauseDur = millis() - pauseStart;
    last_tictoc_millis += pauseDur;
    last_beep_speed_change_millis += pauseDur;
  }

  // Serve next word
  unsigned long off = wordOffsets[wordPos++];
  wordsFile.seek(off);
  String line = wordsFile.readStringUntil('\n');
  line.trim();
  out = line;
  return out.length() > 0;
}

// ===== Beeps =====
void beep_tic()      { 
  if (!muted) 
  tone(SPEAKER_PIN, 300, 30); 
  }
void beep_toc()      { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_times_up() {
  if (!muted) { 
    tone(SPEAKER_PIN, 300, 300); 
    delay(300); 
    tone(SPEAKER_PIN, 300, 300); 
    delay(300); 
    tone(SPEAKER_PIN, 300, 300);
    delay(300);
    tone(SPEAKER_PIN, 300, 300); 
    delay(300); 
    tone(SPEAKER_PIN, 300, 300); 
    delay(300); 
    tone(SPEAKER_PIN, 300, 300); 
    }
  else { delay(900); }
}
void beep_power_on() { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_small()    { if (!muted) tone(SPEAKER_PIN, 300, 30); }
void beep_win_game() {
  if (muted) return;
  for (int i = 0; i < 3; ++i) {
    tone(SPEAKER_PIN, 300, 250);
    delay(100);
    tone(SPEAKER_PIN, 400, 250);
    delay(100);
    tone(SPEAKER_PIN, 500, 250);
    delay(100);
  }
}


// ===== Round flow =====
void showWord(const String &word) {
  String two = format_for_lcd(word);
  if (two.length() == 0) two = format_for_lcd("(too long)");
  showScoresAndText(two);
}

void startRound() {
  lcd.clear();
  // IMPORTANT: do NOT build/reshuffle deck here.
  // We keep the deck persistent across rounds and only rebuild when exhausted.
  gameState = IN_ROUND;
  cur_beep_interval = 0;
  next_is_tic = true;
  last_tictoc_millis = 0;
  last_beep_speed_change_millis = millis();

  String w;
  if (!getNextWordFromDeck(w)) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print(F("No words indexed"));
    lcd.setCursor(0,1); lcd.print(F("Check words.txt"));
    return;
  }
  currentWord = w;
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

  // NOTE: We intentionally do NOT build the deck in setup().
  // This keeps the first batch truly random (we seed & build on first Start press).

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
        if (score_team1 == 7) { 
          lcd.clear(); 
          lcd.setCursor(0,0); 
          lcd.print(F("Team 1 Wins!")); 
          beep_win_game();
          gameState = GAME_DONE; }
        else { 
          String two = format_for_lcd("Press Start"); 
          showScoresAndText(two); 
          }
      }
      if (btnT2.justPressed()) {
        score_team2++; beep_small();
        if (score_team2 == 7) { 
          lcd.clear(); 
          lcd.setCursor(0,0); 
          lcd.print(F("Team 2 Wins!"));
          beep_win_game();
          gameState = GAME_DONE; }
        else { String two = format_for_lcd("Press Start"); 
        showScoresAndText(two); 
        }
      }
      break;

    case IN_ROUND:
      if (btnStart.justPressed()) { endRound(false); break; } // stop early
      if (btnNext.justPressed()) {
        String w;
        if (getNextWordFromDeck(w)) { currentWord = w; showWord(currentWord); }
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
