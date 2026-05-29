/*
  ESP32 Catchphrase Game
  Board: ESP32-2432S028 / 2.8 inch 240x320 touchscreen board (Cheap Yellow Board CYB)

  Hardware used:
    - Built-in ILI9341 TFT screen
    - Built-in XPT2046 resistive touchscreen
    - Built-in microSD/TF card slot
    - Board SPEAK connector / audio on GPIO26
    - External physical Next Word button on GPIO27 to GND

  SD card folder setup:
    /Animals/words.txt
    /Movies/words.txt
    /Geography/words.txt
    /History/words.txt

  Text file rules:
    - Blank lines are skipped
    - Lines starting with # are skipped
    - Each remaining line becomes one clue
*/

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <strings.h>
#include <esp_system.h>

#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#endif

#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// =====================================================
// Board pin setup for ESP32-2432S028
// =====================================================

// ---------- TFT display pins ----------
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   21

// ---------- Touchscreen pins ----------
#define TP_CLK   25
#define TP_CS    33
#define TP_MOSI  32
#define TP_MISO  39
#define TP_IRQ   36

// ---------- SD card pins ----------
#define SD_CS    5
#define SD_MOSI  23
#define SD_SCLK  18
#define SD_MISO  19

// ---------- Game hardware pins ----------
#define BUZZER_PIN       22   // buzzer red wire to IO22, black wire to GND
#define NEXT_BUTTON_PIN  27   // physical Next Word button to GND

// =====================================================
// Game settings
// =====================================================

const int WIN_SCORE = 7;

// If true, changing category resets both team scores.
// I left this false so category changes do NOT reset the score.
const bool RESET_SCORES_ON_CATEGORY_CHANGE = false;

// Original game beep timing copied from your Nano sketch
unsigned long beep_frequency_change_interval_millis = 15000;
unsigned long beep_interval_millis[] = {500, 500, 300, 200};
const int NUM_BEEP_INTERVALS = 4;

// =====================================================
// Touch calibration
// If the touch position is wrong, adjust these values first.
// =====================================================

#define TOUCH_SWAP_XY   0
#define TOUCH_INVERT_X  0
#define TOUCH_INVERT_Y  0

const int TOUCH_RAW_X_MIN = 200;
const int TOUCH_RAW_X_MAX = 3700;
const int TOUCH_RAW_Y_MIN = 240;
const int TOUCH_RAW_Y_MAX = 3800;
const int TOUCH_MIN_Z     = 200;

// =====================================================
// Screen colors
// =====================================================

#define C_BLACK      0x0000
#define C_WHITE      0xFFFF
#define C_RED        0xF800
#define C_GREEN      0x07E0
#define C_BLUE       0x001F
#define C_ORANGE     0xFD20
#define C_YELLOW     0xFFE0
#define C_GRAY       0x8410
#define C_LIGHTGRAY  0xD69A
#define C_DARKGRAY   0x4208

// =====================================================
// Display object
// =====================================================

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  TFT_DC,
  TFT_CS,
  TFT_SCLK,
  TFT_MOSI,
  TFT_MISO,
  HSPI
);

Arduino_GFX *gfx = new Arduino_ILI9341(
  bus,
  TFT_RST,
  3,      // landscape rotation is 1 or 3 (change from 1 to 3 to flip the screen). Also need to change later in the script at "touchscreen.setRotation(3);" and "gfx->setRotation(3);"
  true   // IPS color inversion turned on/off
);

// =====================================================
// Shared auxiliary SPI bus for SD and Touch
// TFT uses HSPI, this uses VSPI and is switched as needed.
// =====================================================

SPIClass auxSPI(VSPI);
XPT2046_Touchscreen touchscreen(TP_CS, TP_IRQ);

enum AuxMode {
  AUX_NONE,
  AUX_SD,
  AUX_TOUCH
};

AuxMode auxMode = AUX_NONE;

// =====================================================
// Screen layout
// =====================================================

int screenW = 320;
int screenH = 240;

const int BTN_R = 25;

const int CAT_BTN_X   = 34;
const int CAT_BTN_Y   = 34;

const int START_BTN_X = 286;
const int START_BTN_Y = 34;

const int TEAM1_BTN_X = 34;
const int TEAM1_BTN_Y = 206;

const int TEAM2_BTN_X = 286;
const int TEAM2_BTN_Y = 206;

const int WORD_AREA_X = 12;
const int WORD_AREA_Y = 68;
const int WORD_AREA_W = 296;
const int WORD_AREA_H = 105;

const int CATEGORY_LIST_X = 18;
const int CATEGORY_LIST_Y = 62;
const int CATEGORY_LIST_W = 244;
const int CATEGORY_ROW_H  = 31;
const int CATEGORY_VISIBLE_ROWS = 4;

// =====================================================
// Game state
// =====================================================

enum GameState {
  READY,
  IN_ROUND,
  GAME_DONE
};

enum ScreenMode {
  SCREEN_GAME,
  SCREEN_CATEGORY
};

GameState gameState = READY;
ScreenMode screenMode = SCREEN_GAME;

int score_team1 = 0;
int score_team2 = 0;

String currentWord = "";

int cur_beep_interval = 0;
bool next_is_tic = true;
unsigned long last_tictoc_millis = 0;
unsigned long last_beep_speed_change_millis = 0;

// =====================================================
// Categories and words
// =====================================================

struct CategoryItem {
  String name;
  String wordFilePath;
};

std::vector<CategoryItem> categories;

// Word storage for the selected category.
// We store the whole words.txt file in one buffer, then store only the
// starting position of each word. This avoids thousands of separate String
// allocations, which can crash/reboot the ESP32 with large files.
char *wordBuffer = nullptr;
uint32_t wordBufferSize = 0;

// Starting position of each word inside wordBuffer.
std::vector<uint32_t> wordStarts;

// Shuffled order of word indexes.
std::vector<uint32_t> wordOrder;

int selectedCategoryIndex = -1;
int loadedCategoryIndex = -1;
int categoryScrollOffset = 0;
int wordIndex = 0;

uint32_t lastServedWordIndex = 0xFFFFFFFF;

// =====================================================
// Debounced physical button
// =====================================================

struct DebouncedButton {
  int pin;
  int lastAdvertised;
  int curAdvertised;
  int lastRead;
  unsigned long lastChange;

  void begin(int p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    lastAdvertised = HIGH;
    curAdvertised = HIGH;
    lastRead = HIGH;
    lastChange = 0;
  }

  void update() {
    int s = digitalRead(pin);
    unsigned long now = millis();

    if (s != lastRead) {
      lastChange = now;
    }

    if (now - lastChange > 50) {
      curAdvertised = s;
    }

    lastRead = s;
  }

  bool justPressed() {
    bool jp = (curAdvertised != lastAdvertised) && (curAdvertised == LOW);
    lastAdvertised = curAdvertised;
    return jp;
  }
};

DebouncedButton btnNext;

// =====================================================
// Buzzer / speaker code
// Copied from original behavior: 300 Hz short beeps.
// =====================================================

const int BUZZER_CHANNEL = 0;

void buzzerBegin() {
  pinMode(BUZZER_PIN, OUTPUT);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 300, 10);
#else
  ledcSetup(BUZZER_CHANNEL, 300, 10);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
#endif
}

void buzzerToneOn(int freq) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, freq);
#else
  ledcWriteTone(BUZZER_CHANNEL, freq);
#endif
}

void buzzerToneOff() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, 0);
#else
  ledcWriteTone(BUZZER_CHANNEL, 0);
#endif
}

void playToneBlocking(int freq, int durationMs) {
  buzzerToneOn(freq);
  delay(durationMs);
  buzzerToneOff();
}

void beep_tic() {
  playToneBlocking(300, 30);
}

void beep_toc() {
  playToneBlocking(300, 30);
}

void beep_small() {
  playToneBlocking(300, 30);
}

void beep_power_on() {
  playToneBlocking(300, 30);
}

void beep_times_up() {
  for (int i = 0; i < 6; i++) {
    playToneBlocking(300, 300);
  }
}

void beep_win_game() {
  for (int i = 0; i < 3; i++) {
    playToneBlocking(300, 250);
    delay(100);
    playToneBlocking(400, 250);
    delay(100);
    playToneBlocking(500, 250);
    delay(100);
  }
}

// =====================================================
// SPI mode switching
// =====================================================

bool beginSDMode() {
  if (auxMode == AUX_SD) return true;

  digitalWrite(TP_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  SD.end();
  auxSPI.end();

  auxSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, auxSPI, 4000000)) {
    auxMode = AUX_NONE;
    return false;
  }

  auxMode = AUX_SD;
  return true;
}

void beginTouchMode() {
  if (auxMode == AUX_SD) {
    SD.end();
  }

  digitalWrite(SD_CS, HIGH);
  digitalWrite(TP_CS, HIGH);

  auxSPI.end();
  auxSPI.begin(TP_CLK, TP_MISO, TP_MOSI, TP_CS);

  touchscreen.begin(auxSPI);
  touchscreen.setRotation(3); // 1 and 3 are for landscape

  auxMode = AUX_TOUCH;
}

// =====================================================
// Text helpers
// =====================================================

int textWidth(const String &s, int textSize) {
  int16_t x1, y1;
  uint16_t w, h;

  gfx->setTextSize(textSize);
  gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);

  return (int)w;
}

String fitTextToWidth(String s, int maxW, int textSize) {
  s.trim();

  if (textWidth(s, textSize) <= maxW) {
    return s;
  }

  while (s.length() > 3 && textWidth(s + "...", textSize) > maxW) {
    s.remove(s.length() - 1);
  }

  return s + "...";
}

void drawCenteredText(const String &s, int centerX, int topY, int textSize, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;

  gfx->setTextSize(textSize);
  gfx->setTextColor(color);
  gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);

  int x = centerX - (int)w / 2 - x1;
  gfx->setCursor(x, topY);
  gfx->print(s);
}

std::vector<String> wrapText(String text, int maxW, int textSize) {
  std::vector<String> lines;

  text.replace('\r', ' ');
  text.replace('\n', ' ');
  text.trim();

  String current = "";
  int pos = 0;

  while (pos < text.length()) {
    while (pos < text.length() && text[pos] == ' ') {
      pos++;
    }

    if (pos >= text.length()) break;

    int nextSpace = text.indexOf(' ', pos);
    if (nextSpace < 0) nextSpace = text.length();

    String word = text.substring(pos, nextSpace);
    String candidate = current.length() == 0 ? word : current + " " + word;

    if (textWidth(candidate, textSize) <= maxW || current.length() == 0) {
      current = candidate;
    } else {
      lines.push_back(current);
      current = word;
    }

    pos = nextSpace + 1;
  }

  if (current.length() > 0) {
    lines.push_back(current);
  }

  if (lines.size() == 0) {
    lines.push_back("");
  }

  return lines;
}

void drawWrappedCenteredText(String text, int x, int y, int w, int h, uint16_t color) {
  int chosenSize = 1;
  std::vector<String> chosenLines;

  for (int size = 4; size >= 1; size--) {
    std::vector<String> lines = wrapText(text, w, size);
    int lineH = 8 * size + 6;
    int totalH = lines.size() * lineH;

    if (totalH <= h) {
      chosenSize = size;
      chosenLines = lines;
      break;
    }

    if (size == 1) {
      chosenSize = 1;
      chosenLines = lines;
    }
  }

  int lineH = 8 * chosenSize + 6;
  int totalH = chosenLines.size() * lineH;
  int startY = y + (h - totalH) / 2;

  for (int i = 0; i < (int)chosenLines.size(); i++) {
    String line = chosenLines[i];
    drawCenteredText(line, x + w / 2, startY + i * lineH, chosenSize, color);
  }
}

// =====================================================
// Drawing buttons and screens
// =====================================================

void drawCircleButton(int x, int y, int r, uint16_t color, const String &label, int labelSize) {
  gfx->fillCircle(x, y, r, color);
  gfx->drawCircle(x, y, r, C_BLACK);

  if (label.length() > 0) {
    drawCenteredText(label, x, y - (4 * labelSize), labelSize, C_WHITE);
  }
}

String selectedCategoryName() {
  if (selectedCategoryIndex < 0 || selectedCategoryIndex >= (int)categories.size()) {
    return "No Category";
  }

  return categories[selectedCategoryIndex].name;
}

String readyMessage() {
  if (categories.size() == 0) {
    return "No categories found on SD card";
  }

  return "Category: " + selectedCategoryName() + "   Press Start";
}

void drawScoreButtons() {
  drawCircleButton(TEAM1_BTN_X, TEAM1_BTN_Y, BTN_R, C_BLUE, String(score_team1), 3);
  drawCircleButton(TEAM2_BTN_X, TEAM2_BTN_Y, BTN_R, C_RED, String(score_team2), 3);
}

void drawTopButtons() {
  if (gameState != IN_ROUND) {
    drawCircleButton(CAT_BTN_X, CAT_BTN_Y, BTN_R, C_ORANGE, "CAT", 1);
  }

  if (gameState == IN_ROUND) {
    drawCircleButton(START_BTN_X, START_BTN_Y, BTN_R, C_RED, "STOP", 1);
  } else {
    drawCircleButton(START_BTN_X, START_BTN_Y, BTN_R, C_GREEN, "GO", 2);
  }
}

void drawGameScreen(const String &message) {
  gfx->fillScreen(C_BLACK);

  drawTopButtons();
  drawScoreButtons();

  gfx->fillRect(WORD_AREA_X, WORD_AREA_Y, WORD_AREA_W, WORD_AREA_H, C_BLACK);
drawWrappedCenteredText(message, WORD_AREA_X, WORD_AREA_Y, WORD_AREA_W, WORD_AREA_H, C_WHITE);
}

void drawSimpleMessage(const String &message) {
  gfx->fillScreen(C_BLACK);
  drawWrappedCenteredText(message, 10, 70, screenW - 20, 100, C_WHITE);
}

void clampCategoryScroll() {
  int maxOffset = (int)categories.size() - CATEGORY_VISIBLE_ROWS;
  if (maxOffset < 0) maxOffset = 0;

  if (categoryScrollOffset < 0) categoryScrollOffset = 0;
  if (categoryScrollOffset > maxOffset) categoryScrollOffset = maxOffset;
}

void makeSelectedCategoryVisible() {
  if (selectedCategoryIndex < 0) return;

  if (selectedCategoryIndex < categoryScrollOffset) {
    categoryScrollOffset = selectedCategoryIndex;
  }

  if (selectedCategoryIndex >= categoryScrollOffset + CATEGORY_VISIBLE_ROWS) {
    categoryScrollOffset = selectedCategoryIndex - CATEGORY_VISIBLE_ROWS + 1;
  }

  clampCategoryScroll();
}

void drawCategoryScreen() {
  gfx->fillScreen(C_BLACK);

  drawTopButtons();
  drawScoreButtons();

  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(78, 14);
  gfx->print("Categories");

  if (categories.size() == 0) {
    drawWrappedCenteredText("No category folders found on SD card", 20, 80, 280, 80, C_BLACK);
    return;
  }

  clampCategoryScroll();

  for (int row = 0; row < CATEGORY_VISIBLE_ROWS; row++) {
    int catIndex = categoryScrollOffset + row;
    if (catIndex >= (int)categories.size()) break;

    int rowX = CATEGORY_LIST_X;
    int rowY = CATEGORY_LIST_Y + row * CATEGORY_ROW_H;

    bool selected = (catIndex == selectedCategoryIndex);

    uint16_t fillColor = selected ? C_YELLOW : C_LIGHTGRAY;

    gfx->fillRoundRect(rowX, rowY, CATEGORY_LIST_W, CATEGORY_ROW_H - 3, 6, fillColor);
    gfx->drawRoundRect(rowX, rowY, CATEGORY_LIST_W, CATEGORY_ROW_H - 3, 6, C_BLACK);

    String shown = fitTextToWidth(categories[catIndex].name, CATEGORY_LIST_W - 18, 2);

    gfx->setTextSize(2);
    gfx->setTextColor(C_BLACK);
    gfx->setCursor(rowX + 9, rowY + 7);
    gfx->print(shown);
  }

  if ((int)categories.size() > CATEGORY_VISIBLE_ROWS) {
  // Up arrow - moved slightly down toward center
  if (categoryScrollOffset > 0) {
    gfx->fillTriangle(292, 90, 276, 112, 308, 112, C_DARKGRAY);
  } else {
    gfx->fillTriangle(292, 90, 276, 112, 308, 112, C_LIGHTGRAY);
  }

  // Down arrow - moved slightly up toward center
  int maxOffset = (int)categories.size() - CATEGORY_VISIBLE_ROWS;
  if (categoryScrollOffset < maxOffset) {
    gfx->fillTriangle(276, 150, 308, 150, 292, 174, C_DARKGRAY);
  } else {
    gfx->fillTriangle(276, 150, 308, 150, 292, 174, C_LIGHTGRAY);
  }
}
}

void redrawCurrentScreen() {
  if (screenMode == SCREEN_CATEGORY && gameState != IN_ROUND) {
    drawCategoryScreen();
    return;
  }

  if (gameState == GAME_DONE) {
    if (score_team1 >= WIN_SCORE) {
      drawGameScreen("Blue Team Wins! Press GO to reset.");
    } else {
      drawGameScreen("Red Team Wins! Press GO to reset.");
    }
    return;
  }

  if (gameState == IN_ROUND) {
    drawGameScreen(currentWord);
    return;
  }

  drawGameScreen(readyMessage());
}

// =====================================================
// SD category scanning and word loading
// =====================================================

String joinPath(const String &dir, const String &file) {
  if (dir == "/") {
    return "/" + file;
  }

  return dir + "/" + file;
}

String baseNameFromPath(String path) {
  path.trim();

  while (path.endsWith("/")) {
    path.remove(path.length() - 1);
  }

  int slash = path.lastIndexOf('/');
  if (slash >= 0) {
    return path.substring(slash + 1);
  }

  return path;
}

String normalizePath(String path) {
  path.trim();

  if (!path.startsWith("/")) {
    path = "/" + path;
  }

  return path;
}

String findTextFileInFolder(const String &folderPath) {
  String p1 = joinPath(folderPath, "words.txt");
  if (SD.exists(p1.c_str())) return p1;

  String p2 = joinPath(folderPath, "WORDS.TXT");
  if (SD.exists(p2.c_str())) return p2;

  File dir = SD.open(folderPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "";
  }

  String found = "";

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;

    if (!entry.isDirectory()) {
      String entryName = String(entry.name());
      String lower = entryName;
      lower.toLowerCase();

      if (lower.endsWith(".txt")) {
        if (!entryName.startsWith("/")) {
          found = joinPath(folderPath, entryName);
        } else {
          found = entryName;
        }

        entry.close();
        break;
      }
    }

    entry.close();
  }

  dir.close();
  return found;
}

bool categorySortCompare(const CategoryItem &a, const CategoryItem &b) {
  return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

bool scanCategoriesFromSD() {
  categories.clear();

  if (!beginSDMode()) {
    return false;
  }

  // Optional backward compatibility: root /words.txt becomes "Default"
  if (SD.exists("/words.txt") || SD.exists("/WORDS.TXT")) {
    CategoryItem rootCat;
    rootCat.name = "Default";

    if (SD.exists("/words.txt")) {
      rootCat.wordFilePath = "/words.txt";
    } else {
      rootCat.wordFilePath = "/WORDS.TXT";
    }

    categories.push_back(rootCat);
  }

  File root = SD.open("/");
  if (!root) {
    return false;
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    if (entry.isDirectory()) {
      String folderPath = normalizePath(String(entry.name()));
      String folderName = baseNameFromPath(folderPath);
      String txtPath = findTextFileInFolder(folderPath);

      if (txtPath.length() > 0) {
        CategoryItem item;
        item.name = folderName;
        item.wordFilePath = txtPath;
        categories.push_back(item);
      }
    }

    entry.close();
  }

  root.close();

  std::sort(categories.begin(), categories.end(), categorySortCompare);

  if (categories.size() > 0) {
    selectedCategoryIndex = 0;
    categoryScrollOffset = 0;
  } else {
    selectedCategoryIndex = -1;
  }

  return true;
}

void clearLoadedWords() {
  if (wordBuffer != nullptr) {
    free(wordBuffer);
    wordBuffer = nullptr;
  }

  wordBufferSize = 0;
  wordStarts.clear();
  wordOrder.clear();
  wordIndex = 0;
  loadedCategoryIndex = -1;
  lastServedWordIndex = 0xFFFFFFFF;
}

void shuffleWords() {
  wordOrder.clear();

  uint32_t count = wordStarts.size();
  if (count == 0) return;

  wordOrder.reserve(count);

  for (uint32_t i = 0; i < count; i++) {
    wordOrder.push_back(i);

    if ((i % 200) == 0) {
      yield();
    }
  }

  for (int32_t i = (int32_t)wordOrder.size() - 1; i > 0; i--) {
    uint32_t j = (uint32_t)random(i + 1);

    uint32_t temp = wordOrder[i];
    wordOrder[i] = wordOrder[j];
    wordOrder[j] = temp;

    if ((i % 200) == 0) {
      yield();
    }
  }

  // Avoid showing the same word twice in a row after reshuffle.
  if (lastServedWordIndex != 0xFFFFFFFF && wordOrder.size() > 1) {
    if (wordOrder[0] == lastServedWordIndex) {
      uint32_t swapIndex = (uint32_t)random(1, wordOrder.size());

      uint32_t temp = wordOrder[0];
      wordOrder[0] = wordOrder[swapIndex];
      wordOrder[swapIndex] = temp;
    }
  }

  wordIndex = 0;
}

bool loadSelectedCategoryWords() {
  if (selectedCategoryIndex < 0 || selectedCategoryIndex >= (int)categories.size()) {
    return false;
  }

  // If this category is already loaded, do not reload it.
  if (loadedCategoryIndex == selectedCategoryIndex && wordBuffer != nullptr && wordStarts.size() > 0 && wordOrder.size() > 0) {
    return true;
  }

  drawSimpleMessage("Loading " + categories[selectedCategoryIndex].name + "...");

  if (!beginSDMode()) {
    beginTouchMode();
    drawSimpleMessage("SD card failed");
    delay(1200);
    return false;
  }

  clearLoadedWords();

  String path = categories[selectedCategoryIndex].wordFilePath;

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    beginTouchMode();
    drawSimpleMessage("Could not open " + path);
    delay(1500);
    return false;
  }

  uint32_t fileSize = file.size();

  if (fileSize == 0) {
    file.close();
    beginTouchMode();
    drawSimpleMessage("Empty words file");
    delay(1500);
    return false;
  }

  Serial.print("Loading file: ");
  Serial.println(path);
  Serial.print("File size: ");
  Serial.println(fileSize);
  Serial.print("Free heap before malloc: ");
  Serial.println(ESP.getFreeHeap());

  wordBuffer = (char *)malloc(fileSize + 1);

  if (wordBuffer == nullptr) {
    file.close();
    beginTouchMode();
    drawSimpleMessage("Not enough memory");
    delay(2000);
    return false;
  }

  size_t bytesRead = file.readBytes(wordBuffer, fileSize);
  file.close();

  wordBuffer[bytesRead] = '\0';
  wordBufferSize = bytesRead;

  Serial.print("Bytes read: ");
  Serial.println(bytesRead);
  Serial.print("Free heap after file read: ");
  Serial.println(ESP.getFreeHeap());

  // Estimate word count only to reduce vector reallocations.
  uint32_t estimatedWords = bytesRead / 8;
  if (estimatedWords < 100) estimatedWords = 100;
  if (estimatedWords > 12000) estimatedWords = 12000;
  wordStarts.reserve(estimatedWords);

  uint32_t pos = 0;

  while (pos < bytesRead) {
    uint32_t lineStart = pos;

    while (pos < bytesRead && wordBuffer[pos] != '\n' && wordBuffer[pos] != '\r') {
      pos++;
    }

    uint32_t lineEnd = pos;

    while (pos < bytesRead && (wordBuffer[pos] == '\n' || wordBuffer[pos] == '\r')) {
      wordBuffer[pos] = '\0';
      pos++;
    }

    // Remove UTF-8 BOM if the file has one.
    if (lineStart == 0 && bytesRead >= 3) {
      if ((uint8_t)wordBuffer[0] == 0xEF &&
          (uint8_t)wordBuffer[1] == 0xBB &&
          (uint8_t)wordBuffer[2] == 0xBF) {
        lineStart += 3;
      }
    }

    // Trim leading spaces/tabs.
    while (lineStart < lineEnd &&
           (wordBuffer[lineStart] == ' ' || wordBuffer[lineStart] == '\t')) {
      lineStart++;
    }

    // Trim trailing spaces/tabs.
    while (lineEnd > lineStart &&
           (wordBuffer[lineEnd - 1] == ' ' || wordBuffer[lineEnd - 1] == '\t')) {
      wordBuffer[lineEnd - 1] = '\0';
      lineEnd--;
    }

    // Skip blank lines and comment lines.
    if (lineStart < lineEnd && wordBuffer[lineStart] != '#') {
      wordStarts.push_back(lineStart);
    }

    if ((wordStarts.size() % 200) == 0) {
      yield();
    }
  }

  beginTouchMode();

  if (wordStarts.size() == 0) {
    clearLoadedWords();
    drawSimpleMessage("No words found in " + categories[selectedCategoryIndex].name);
    delay(1500);
    return false;
  }

  Serial.print("Words loaded: ");
  Serial.println(wordStarts.size());
  Serial.print("Free heap before shuffle: ");
  Serial.println(ESP.getFreeHeap());

  randomSeed(esp_random());

  drawSimpleMessage("Shuffling " + String(wordStarts.size()) + " words...");
  shuffleWords();

  Serial.print("Free heap after shuffle: ");
  Serial.println(ESP.getFreeHeap());

  loadedCategoryIndex = selectedCategoryIndex;
  wordIndex = 0;

  return true;
}

bool getNextWord(String &out) {
  if (wordBuffer == nullptr || wordStarts.size() == 0) {
    return false;
  }

  if (wordOrder.size() == 0 || wordIndex >= (int)wordOrder.size()) {
    shuffleWords();
  }

  if (wordOrder.size() == 0) {
    return false;
  }

  uint32_t realWordIndex = wordOrder[wordIndex];
  wordIndex++;

  uint32_t startPos = wordStarts[realWordIndex];

  out = String(wordBuffer + startPos);
  lastServedWordIndex = realWordIndex;

  return true;
}

// =====================================================
// Touch handling
// =====================================================

bool touchWasDown = false;
unsigned long lastTouchPressMs = 0;

bool readTouchPoint(int &touchX, int &touchY) {
  if (auxMode != AUX_TOUCH) return false;

  if (!touchscreen.touched()) {
    return false;
  }

  TS_Point raw = touchscreen.getPoint();

  if (raw.z < TOUCH_MIN_Z) {
    return false;
  }

  int rawX = raw.x;
  int rawY = raw.y;

#if TOUCH_SWAP_XY
  int temp = rawX;
  rawX = rawY;
  rawY = temp;
#endif

  int sx = map(rawX, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, screenW - 1);
  int sy = map(rawY, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, screenH - 1);

#if TOUCH_INVERT_X
  sx = screenW - 1 - sx;
#endif

#if TOUCH_INVERT_Y
  sy = screenH - 1 - sy;
#endif

  sx = constrain(sx, 0, screenW - 1);
  sy = constrain(sy, 0, screenH - 1);

  touchX = sx;
  touchY = sy;

  return true;
}

bool getTouchPress(int &touchX, int &touchY) {
  int nowX = 0;
  int nowY = 0;

  bool down = readTouchPoint(nowX, nowY);
  unsigned long now = millis();

  if (down && !touchWasDown && now - lastTouchPressMs > 120) {
    touchWasDown = true;
    lastTouchPressMs = now;

    touchX = nowX;
    touchY = nowY;

    return true;
  }

  if (!down) {
    touchWasDown = false;
  }

  return false;
}

bool pointInCircle(int px, int py, int cx, int cy, int r) {
  long dx = px - cx;
  long dy = py - cy;
  return dx * dx + dy * dy <= (long)r * (long)r;
}

// =====================================================
// Game logic
// =====================================================

void startRound() {
  if (!loadSelectedCategoryWords()) {
    gameState = READY;
    screenMode = SCREEN_GAME;
    redrawCurrentScreen();
    return;
  }

  String w;
  if (!getNextWord(w)) {
    gameState = READY;
    screenMode = SCREEN_GAME;
    drawGameScreen("No words found. Check the SD card text file.");
    return;
  }

  currentWord = w;

  gameState = IN_ROUND;
  screenMode = SCREEN_GAME;

  cur_beep_interval = 0;
  next_is_tic = true;
  last_tictoc_millis = 0;
  last_beep_speed_change_millis = millis();

  redrawCurrentScreen();
}

void endRound(bool timesUp) {
  if (timesUp) {
    beep_times_up();
  }

  gameState = READY;
  screenMode = SCREEN_GAME;

  drawGameScreen("Round Over. Add a score or press Start.");
}

void resetGame() {
  score_team1 = 0;
  score_team2 = 0;
  gameState = READY;
  screenMode = SCREEN_GAME;
  redrawCurrentScreen();
}

void addScoreToTeam(int team) {
  if (gameState != READY) return;

  if (team == 1) {
    score_team1++;
  } else {
    score_team2++;
  }

  beep_small();

  if (score_team1 >= WIN_SCORE || score_team2 >= WIN_SCORE) {
    gameState = GAME_DONE;
    screenMode = SCREEN_GAME;
    redrawCurrentScreen();
    beep_win_game();
    return;
  }

  redrawCurrentScreen();
}

void do_tic_toc() {
  unsigned long now = millis();

  if (now - last_beep_speed_change_millis > beep_frequency_change_interval_millis) {
    last_beep_speed_change_millis = now;

    cur_beep_interval++;

    if (cur_beep_interval >= NUM_BEEP_INTERVALS) {
      endRound(true);
      return;
    }
  }

  if (now - last_tictoc_millis > beep_interval_millis[cur_beep_interval]) {
    if (next_is_tic) {
      beep_tic();
    } else {
      beep_toc();
    }

    next_is_tic = !next_is_tic;
    last_tictoc_millis = now;
  }
}

// =====================================================
// Touch action handling
// =====================================================

void selectCategoryIndex(int index) {
  if (index < 0 || index >= (int)categories.size()) return;

  if (selectedCategoryIndex != index) {
    selectedCategoryIndex = index;

    if (RESET_SCORES_ON_CATEGORY_CHANGE) {
      score_team1 = 0;
      score_team2 = 0;
    }

    makeSelectedCategoryVisible();
  }

  redrawCurrentScreen();
}

void handleCategoryListTouch(int x, int y) {
  // Scroll arrows
  if ((int)categories.size() > CATEGORY_VISIBLE_ROWS) {
    int maxOffset = (int)categories.size() - CATEGORY_VISIBLE_ROWS;

    // Up arrow area - matches new lower arrow position
if (x >= 270 && y >= 78 && y <= 125) {
  if (categoryScrollOffset > 0) {
    categoryScrollOffset--;
    redrawCurrentScreen();
  }
  return;
}

// Down arrow area - matches new higher arrow position
if (x >= 270 && y >= 138 && y <= 188) {
  if (categoryScrollOffset < maxOffset) {
    categoryScrollOffset++;
    redrawCurrentScreen();
  }
  return;
}
  }

  // Category rows
  if (x >= CATEGORY_LIST_X && x <= CATEGORY_LIST_X + CATEGORY_LIST_W) {
    if (y >= CATEGORY_LIST_Y && y < CATEGORY_LIST_Y + CATEGORY_VISIBLE_ROWS * CATEGORY_ROW_H) {
      int row = (y - CATEGORY_LIST_Y) / CATEGORY_ROW_H;
      int index = categoryScrollOffset + row;

      if (index >= 0 && index < (int)categories.size()) {
        selectCategoryIndex(index);
      }
    }
  }
}

void handleStartButton() {
  if (gameState == IN_ROUND) {
    endRound(false);
    return;
  }

  if (gameState == GAME_DONE) {
    resetGame();
    return;
  }

  startRound();
}

void handleTouch(int x, int y) {
  // During active gameplay, only the red Stop button works.
  // Category button is hidden, and score buttons do not score during a round.
  if (gameState == IN_ROUND) {
    if (pointInCircle(x, y, START_BTN_X, START_BTN_Y, BTN_R + 8)) {
      handleStartButton();
    }

    return;
  }

  // Top-right green Start button
  if (pointInCircle(x, y, START_BTN_X, START_BTN_Y, BTN_R + 8)) {
    handleStartButton();
    return;
  }

  // Top-left orange category button
  if (pointInCircle(x, y, CAT_BTN_X, CAT_BTN_Y, BTN_R + 8)) {
    if (screenMode == SCREEN_CATEGORY) {
      screenMode = SCREEN_GAME;
    } else {
      screenMode = SCREEN_CATEGORY;
      makeSelectedCategoryVisible();
    }

    redrawCurrentScreen();
    return;
  }

  // Score buttons only work before game is done
  if (gameState == READY) {
    if (pointInCircle(x, y, TEAM1_BTN_X, TEAM1_BTN_Y, BTN_R + 8)) {
      addScoreToTeam(1);
      return;
    }

    if (pointInCircle(x, y, TEAM2_BTN_X, TEAM2_BTN_Y, BTN_R + 8)) {
      addScoreToTeam(2);
      return;
    }
  }

  // Category list touch
  if (screenMode == SCREEN_CATEGORY && gameState == READY) {
    handleCategoryListTouch(x, y);
    return;
  }
}

// =====================================================
// Setup and loop
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.print("Reset reason: ");
  Serial.println(esp_reset_reason());
  Serial.print("Free heap at startup: ");
  Serial.println(ESP.getFreeHeap());

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  btnNext.begin(NEXT_BUTTON_PIN);
  buzzerBegin();

  gfx->begin();
  gfx->setRotation(3);
  gfx->setTextWrap(false);

  screenW = gfx->width();
  screenH = gfx->height();

  drawSimpleMessage("Starting...");
  beep_power_on();
  delay(300);

  drawSimpleMessage("Scanning SD card...");

  if (!scanCategoriesFromSD()) {
    drawSimpleMessage("SD card failed. Check card format and slot.");
    while (true) {
      delay(1000);
    }
  }

  beginTouchMode();

  if (categories.size() == 0) {
    drawSimpleMessage("No categories found. Add folders with words.txt files.");
    while (true) {
      delay(1000);
    }
  }

  selectedCategoryIndex = 0;
  loadedCategoryIndex = -1;
  score_team1 = 0;
  score_team2 = 0;
  gameState = READY;
  screenMode = SCREEN_GAME;

  redrawCurrentScreen();
}

void loop() {
  btnNext.update();

int touchX = 0;
int touchY = 0;

if (getTouchPress(touchX, touchY)) {
  handleTouch(touchX, touchY);
}

  if (gameState == IN_ROUND) {
    if (btnNext.justPressed()) {
      String w;

      if (getNextWord(w)) {
        currentWord = w;
        redrawCurrentScreen();
      }
    }

    do_tic_toc();
  }

  delay(5);
}