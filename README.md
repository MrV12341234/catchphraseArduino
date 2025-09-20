# catchphraseArduino

Catchphrase (No Categories)
==Hardware needed:
    - Arduino Uno (or nano)
    - 1602A LCD screen (parallel) on pins RS=8, E=9, D4=A0(14), D5=A1(15), D6=A2(16), D7=A3(17)
    - SD TF card reader (CS=10, MOSI=11, MISO=12, SCK=13)
    - 9v or 12v piezo Buzzer
    - 5 Buttons:
        START/STOP = D2
        TEAM1      = D3
        TEAM2      = D4
        NEXT       = D5
        CATEGORY   = D6  (repurposed as MUTE toggle in my script since no categories)

==Wiring==

Arduino ==> Device

1.Buttons
START/STOP(D2)==>one side of button. Other side of button to ground
TEAM1(D3)==>one side of button. Other side of button to ground
TEAM2(D4)==>one side of button. Other side of button to ground
NEXT(D5)==>one side of button. Other side of button to ground
CATEGORY(D6)==>one side of button. Other side of button to ground

2.SDTF SD card reader
GND ==> GND
5v ==> VCC (some card readers require 3.3v. Mine used 5v)
D10 ==> CS
D11 ==> MOSI
D12 ==> MISO
D13 ==> SCK

3. 1602A LCD screen
LCD pins are typically labeled: VSS, VDD, VO, RS, RW, E, D0–D7, A (LED+), K (LED−)
GND ==> VSS (if screen is dim or not coming into focus, add 2nd ground here)
5v ==> VDD
5v ==> one side of potentiometer. Other side of potentiometer goes to ground
middle wire of 10k potentiometer ==> VO (contrast)
D8 ==> RS
GND ==> RW
D9 ==> E
Data lines (4-bit mode; D0–D3 unused):
A0 (Arduino pin 14) ==> D4 (on LCD)  
A1 (Arduino pin 15) ==> D5 (on LCD)
A2 (Arduino pin 16) ==> D6 (on LCD)
A3 (Arduino pin 17) ==> D7 (on LCD)
A4 (Arduino pin 18) ==> 220 ohm resister ==> A pin (on LCD). this is Backlight control on A4 (18). 220 ohm resiter between this pin (A4) and LCD pin A
GND ==> K (on LCD)

4. Piezo buzzer
D7 ==> buzzer positive
Gnd ==> buzzer negative




==Creating the text file==
The text file (notepad) on the sd card must be called "words.txt"


Blank lines or lines starting with # are ignored.

Format a clue to fit: TOP Row=14 chars, BOTTOM=16 chars.
Returns a single String with length TOP_TEXT_LEN + BOTTOM_TEXT_LEN.
First 14 chars -> top text window; last 16 chars -> bottom line.
Splits a phrase at a space if total phrase is longer than the top row length (14).
If single word is longer than 14 letters, but no longer than 16, it is displayed on bottom line 
and top line is left empty (except the scores)
Any words 17+ letters is skipped (small delay in gameplay when game comes upon a long word).
