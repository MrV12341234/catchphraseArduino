// cat_cluesino 
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cat_clues.h"
#include "serial.h"

// File format (fixed width 25 chars per row):
// line 0: "<NUM_CATEGORIES>"  (e.g., "6")
// lines 1..N: category names (includes "Everything" in the header list)
// then a blank 25-char line
// then 5 clue blocks, each separated by a blank line.
// We synthesize categories[0] = "Everything" (union of all clues).
// The 5 real categories from file fill categories[1..5] in order.

int NUM_CATEGORIES;

char ** categories = 0;
unsigned long * category_offsets = 0;
unsigned long * category_len = 0;
char * curline = new char[LINELEN + 1];

static char * rtrim(char * strin) {
  for (int i = strlen(strin) - 1; i >= 0; i--) {
    if (strin[i] == '\0') continue;
    if (strin[i] == ' ' || strin[i] == '\t' || strin[i] == '\n' || strin[i] == '\r') {
      strin[i] = '\0';
    } else {
      return strin;
    }
  }
  return strin;
}

File readFile(const char * filename) {

  File infile;
  int curcategory = 1; // we will fill offsets for categories[1..]; 0 is union "Everything"

  memset(&curline[0], 0, LINELEN + 1);

  infile = SD.open(filename, FILE_READ);
  if (!infile) {
    println("Failed to open clues.txt");
    return File(); // invalid
  }

  // Read NUM_CATEGORIES from header (includes "Everything")
  infile.read(curline, LINELEN);
  int file_num_categories = atoi(rtrim(curline));  // e.g., 6

  // Total exposed categories [0..NUM_CATEGORIES-1]:
  // 0 is synthesized "Everything"; 1..(NUM_CATEGORIES-1) are the 5 real categories
  NUM_CATEGORIES = file_num_categories; // DO NOT add +1
  print("Categories (incl Everything): ");
  println(NUM_CATEGORIES);

  // Allocate arrays
  categories       = (char **)malloc(NUM_CATEGORIES * sizeof(char *));
  category_offsets = (unsigned long *)malloc(NUM_CATEGORIES * sizeof(unsigned long));
  category_len     = (unsigned long *)malloc(NUM_CATEGORIES * sizeof(unsigned long));

  // categories[0] = union "Everything"
  categories[0] = (char *)malloc(sizeof(char) * (LINELEN));
  strcpy(categories[0], "Everything");
  category_len[0] = 0;
  category_offsets[0] = 0;

  // Read the next file_num_categories header lines.
  // Skip the "Everything" line from the file so categories[1..] are the 5 real ones.
  int i = 1;
  for (int readCount = 0; readCount < file_num_categories; ++readCount) {
    memset(&curline[0], 0, LINELEN + 1);
    infile.read(curline, LINELEN);
    char * name = rtrim(curline);
    if (strcmp(name, "Everything") == 0) {
      continue; // skip duplicate header entry
    }
    if (i < NUM_CATEGORIES) {
      categories[i] = (char *)malloc(sizeof(char) * (LINELEN));
      strcpy(categories[i], name);
      println(categories[i]);
      ++i;
    }
  }

  // Scan the clue region; blank 25-char lines separate category blocks.
  while (infile.available()) {
    infile.read(curline, LINELEN);

    // Blank line indicates a category boundary; position() is AFTER this line,
    // which is exactly the start of the next block.
    if (strlen(rtrim(curline)) == 0 && curcategory < NUM_CATEGORIES) {
      category_offsets[curcategory] = infile.position();

      // If this wasn't the first real category, we can compute the previous length now.
      if (curcategory > 1) {
        category_len[curcategory - 1] =
          ((category_offsets[curcategory] - category_offsets[curcategory - 1]) / LINELEN) - 1; // -1 for the blank line
        category_len[0] += category_len[curcategory - 1]; // accumulate for "Everything"
      }
      curcategory++;
    }
  }

  // Close off the last category block (no trailing blank line to subtract)
  category_len[curcategory - 1] =
      (infile.position() - category_offsets[curcategory - 1]) / LINELEN;
  category_len[0] += category_len[curcategory - 1];

  return infile; // keep open; caller seeks into it
}

// Given category [0..NUM_CATEGORIES-1], return a random clue pointer.
char * get_clue(int category, File cluefile) {

  unsigned long seekpos = 0;
  unsigned long curclue = 0;

  // If Everything, pick a random real category (1..NUM_CATEGORIES-1)
  if (category == 0) {
    category = random(1, NUM_CATEGORIES); // Arduino RNG (seeded via randomSeed)
  }

  // Guard
  if (category < 1 || category >= NUM_CATEGORIES) {
    curline[0] = '\0';
    return curline;
  }

  curclue = (unsigned long)random((long)category_len[category]);
  seekpos = category_offsets[category] + curclue * LINELEN;

  cluefile.seek(seekpos);
  cluefile.read(curline, LINELEN);

  return rtrim(curline);
}

String get_clue_as_string(int category, File cluefile) {
  return String(get_clue(category, cluefile));
}

String get_category_as_string(int category) {
  if (category >= 0 && category < NUM_CATEGORIES) {
    return String(categories[category]);
  }
  return String("");
}


