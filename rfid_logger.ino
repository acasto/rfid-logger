#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_PN532.h>
#include <RTClib.h>
#include <toneAC.h>
#include <FastLED.h>

/**
 * Libraries Used
 * (all should be available through the arduino library manager)
 * 
 * SD - https://github.com/arduino-libraries/SD
 * RTClib - https://github.com/adafruit/RTClib
 * PN532 NFS/RFID - https://github.com/adafruit/Adafruit-PN532
 * toneAC - https://bitbucket.org/teckel12/arduino-toneac/wiki/Home
 * fastLED - http://fastled.io/
 * /

/**
 *  Pins used
 * 
 *  analog:
 *          4, 5 - I2C for RFID shield
 *  digital: 
 *          2 - IRQ for RFID shield
 *          3 - reset for RFID shield (not connected by default on the shield)
 *          7 - LED 
 *          8 - chip select for SD card
 *          9, 10 - PWM pins to differential drive buzzer (toneAC)
 *          11, 12, 13 - SPI for SD card
 */

/** RFID stuff **/
// I2C uses analog pins 4 (SDA) and 5 (SCL)
#define PN532_IRQ   (2)
#define PN532_RESET (3)  // Not connected by default on the NFC Shield
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
uint32_t lastCardID; // global scope to survive the loop

/** LED stuff - using fastLED library **/
#define LED_PIN 7
#define LED_COUNT 1
CRGB leds[LED_COUNT];
CRGB def_color = CRGB::Blue;
uint8_t def_bright = 30;

/** RTC stuff **/
RTC_PCF8523 rtc;

/** SD stuff **/
// NOTE - SPI uses pins 11,12, and 13
// ToDo: Modify the readConfig function to not need these temp char arrays 
const uint8_t chipSelect = 8;
#define CHIP_SELECT 8
char config_file[13] = "CONFIG.txt";
char output_file[13] = "DATA.txt";
char acl_file[13] = "ACL.txt";
bool acl = false; // flag for acl mode
char acl_temp[2] = ""; // not really temp since global,but need to fetch string from file to set bool above
char ral_temp[2] = ""; // needed to fetch as string from conf, will convert to int
uint8_t required_access_level = 0;
bool acl_go = false;
char vol_temp[3] = "";
uint8_t vol = 10; 

/** Debug stuff **/
#define SERIAL_DEBUG_ENABLED 1
#define BAUD 115200

#ifdef SERIAL_DEBUG_ENABLED
  #define DebugPrint(x)  Serial.print (x)
  #define DebugPrintln(x) Serial.println (x)
#else
  #define DebugPrint(x)
  #define DebugPrintln(x)
#endif

void setup() {
  
  // Initialize Serial
  #ifdef SERIAL_DEBUG_ENABLED
    Serial.begin(BAUD);
    if (Serial) {
      DebugPrintln(F("Serial Ready!"));
    }
  #endif

  // Initialize LED stuff
  FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, LED_COUNT);

  // Initialize RTC stuff
  if (! rtc.begin()) {
    DebugPrintln(F("Couldn't find RTC"));
    rtc_failure();
  }
  // If RTC not initialized go ahead and set with compile date and time
  if (! rtc.initialized()) {
    DebugPrintln(F("RTC is NOT running!"));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  // added to adjust time if clock is skewed due to flashing boards before attaching RTC
  // Note: if this runs continually make sure you installed the battery for the RTC
  DateTime cTime = (DateTime(__DATE__, __TIME__));
  DateTime rtcTime = rtc.now();
  if ( rtcTime.unixtime() < (cTime.unixtime()-60) ) {
    DebugPrintln(F("Clock skewed. Adjusting."));
    toneAC(400,vol,500,false);
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Initialize SD stuff
  DebugPrintln(F("Initializing SD card... "));
  if (!SD.begin(CHIP_SELECT)) {
    DebugPrintln(F("Card failed, or not present"));
    card_problem();
  }
  DebugPrintln(F("Card initialized"));
 
  // Get config options
  if(SD.exists(config_file)) {
    DebugPrintln(F("Config file found, processing..."));
    // get the output filename 
    readConfig(config_file, output_file, "output_file", (sizeof(output_file)/sizeof(output_file[0])));
    // check if in acl mode
    readConfig(config_file, acl_temp, "acl_mode", (sizeof(acl_temp)/sizeof(acl_temp[0])));
    if (acl_temp[0] == '1' || acl_temp[0] == 't' || acl_temp[0] == 'T') {
      acl = true; // enable acl mode
      acl_enabled(); // beeps or blinks to inidicate acl mode enabled 
      DebugPrintln(F("ACL mode enabled, checking access level"));
      // check to see if access levels are being used
      readConfig(config_file, ral_temp, "required_access_level", (sizeof(ral_temp)/sizeof(ral_temp[0])));
      if (ral_temp[0]) {
        required_access_level = atoi(ral_temp);
        DebugPrintln(F("Required Access Level: "));
        DebugPrintln(required_access_level);
      }
    }
    // get the buzzer volume
    readConfig(config_file, vol_temp, "volume", (sizeof(vol_temp)/sizeof(vol_temp[0])));
    if (vol_temp[0]) {
      vol = atoi(vol_temp);
    }
  } else {
    DebugPrint(F("Can't open config file: "));
    DebugPrintln(config_file);
  }

  // Initialize RFID stuff
  nfc.begin();
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(0x01);

  // beeps and blinks if things are good to go
  good_to_go();
  DebugPrintln(F("Starting loop()"));
}

void loop() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uidLength;                        // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
    
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 1000);
  
  if (success) {
    // limit our action to 4 byte classic UID since we haven't tested against tags with longer UIDs
    if (uidLength == 4) {
      uint32_t cardID = uid[0];
      cardID <<= 8;
      cardID |= uid[1];
      cardID <<= 8;
      cardID |= uid[2];  
      cardID <<= 8;
      cardID |= uid[3]; 

      // If ACL mode is enabled run the check
      if (acl) {
        checkAuth(cardID);
      }
    
      if (cardID != lastCardID) {   // prevent logging the same tag twice in a row
        lastCardID = cardID;
        DateTime now = rtc.now();

        DebugPrint(now.unixtime());
        DebugPrint(F(","));
        DebugPrintln(cardID);
        
        File outFile = SD.open(output_file, FILE_WRITE);
        if (outFile) {
          outFile.print(now.unixtime());
          outFile.print(',');
          if (acl) {
            outFile.print(cardID);
            outFile.print(',');
            if (acl_go) {
              outFile.println('1');
            } else {
              outFile.println('0');
            }
          } else {
            outFile.println(cardID);
          }
          outFile.close();
          if (!acl) {
            entry_logged();
          }
        } else {
          DebugPrintln(F("Error opening file"));
          file_problem();
        }
      } else {
          if (!acl) {
            same_card();
          }
      }
    }
  }
}

void blinkLED(CRGB color, int count, int time) {
  uint8_t i = 0;
  while ( i < count) {
    leds[0] = color;
    FastLED.show();
    delay(time);
    leds[0] = CRGB::Black;
    FastLED.show();
    delay(time);
    i++;
  }
  if (def_bright > 0) {
    leds[0] = def_color;  
    leds[0] %= def_bright;
    FastLED.show();
  }
}

void readConfig(char *conf, char *value, char setting[], int length) {
  char buf;
  char tmpSetting[strlen(setting)] = "";
  char tmpValue[length] = "";
  bool isSetting = true;
  bool isValue = false;
  bool isComment = false;
  uint8_t  s = 0;
  uint8_t  v = 0;

  File cfile = SD.open(conf);
  if (cfile) {
    while (cfile.available()) {
      buf = cfile.read();
        // if newline process and reset
        if (buf == '\n' || buf == '\r') {
            tmpSetting[s] = '\0';
          tmpValue[v] = '\0';
          if (strcmp(setting, tmpSetting) == 0) { 
            break; 
          }
          isSetting = true;
          isValue = false;
          isComment = false;
          s = 0;
          v = 0;
        }
        // handle comment
        if (buf == '#') {
          isComment = true;
          isSetting = false;
          isValue = false;
        }
        if (isComment) {
          continue;
        }
        // drop spaces
        if (isSpace(buf)) {
          continue;
        }
        // handle equals
        if (buf == '=') {
          isSetting = false;
          isValue = true;
          continue;
        }
        // process setting
        if (isSetting) {
          if (s < strlen(setting)) {
            tmpSetting[s] = buf;
            s++;
          }
        }
        // process value
        if (isValue) {
          if (v < (length-1)) {
            tmpValue[v] = buf;
            v++;
          }
        }

    }
    // handle null termination in case of no newline
    if (tmpValue[length-1] != '\0') {
      tmpValue[length-1] = '\0';
    }
    if (strcmp(setting, tmpSetting) == 0) { 
      for (uint8_t i = 0; i < length; i++) {
        value[i] = tmpValue[i]; 
      }
    }    
    cfile.close();
  }

}

void checkAuth(uint32_t cardID) {
  char buf, p;
  char idbuf[11] = "";
  char aclbuf[2] = "";
  uint32_t listID;
  uint8_t accessLevel;
  uint8_t d = 0;
  bool go = 0;
  bool comma = 0;

  char s_uid[11];
  ultoa(cardID, s_uid, 10);
  bool ff = 0; // fast-forward

  File cfile = SD.open(acl_file);
  if (cfile) {
    while (cfile.available()) {
      buf = cfile.read();

      // if \r then expect an \n
      if (buf == '\r') {
        continue;
      }
      
      // we've reached the end of the line
      if (buf == '\n') {
        // if fast-foward was set then there is no match, reset and continue
        if (ff) {
         d = 0;
         ff = 0;
         continue;
        }
        // if we've come this far we have a match 
        // if the comma flag is set then check the access level
        if (comma) {
          aclbuf[1] = '\0';
          accessLevel = atoi(aclbuf);
          if (required_access_level > 0) {
            if (accessLevel >= required_access_level) {
              go = true;
            } else {
            go = false;
            break;
            }
          } 
          go = true;
          break;
        } else {
          // if required_access_level is being used then there must be a comma, if we 
          // got here then there is no comma and so access can not be granted
          if (required_access_level > 0) {
            go = false;
            break;
          }
          accessLevel = 0;
          idbuf[d] = '\0';
          listID = strtoul(idbuf, p, 10);
          go = true;
          break;
        }
      }

      // if fast-forward is set don't bother with the rest for now
      if (ff) {
        continue;
      }

      // if there is a comma then we've matched the ID and need to 
      // process the access level next
      if (buf == ',') {
        idbuf[d] = '\0';
        listID = strtoul(idbuf, p, 10);
        comma = 1;
        continue;
      }

      // if we have the matching ID get the access level
      if (comma) {
        aclbuf[0] = buf;
        continue;
      }
        
      // check the current character for a match, if not activate fast-forward  
      if (s_uid[d] == buf) { 
        idbuf[d] = buf;
        d++;
      } else {
       ff = 1;
      }   

    }
    cfile.close();

    if (go) {
      acl_go = true;
      access_granted();
    } else {
      acl_go = false;
      access_denied();
    }
  }

}

/**
 * Functions for blinks and beeps
 */

void access_granted() {
  toneAC(4200,vol,300,true);
  blinkLED(CRGB::Green, 4, 100);
}

void access_denied() {
  blinkLED(CRGB::Red,2,150);
  //bombed();
  game_over();
}

void bombed() {
    int i=1400;
    while (i>900) {
      toneAC(i,vol,50,false);
      i=i-15;
    }
    toneAC(100,vol,1300,false);
}

void game_over() {
    for(double wah=0; wah<4; wah+=6.541){
      toneAC(440+wah, vol, 50, false);
    }
    toneAC(466.164,vol,100,false);
    delay(80);
    for(double wah=0; wah<5; wah+=4.939){
      toneAC(415.305+wah,vol,50,false);
    }
    toneAC(440,vol,100,false);
    delay(80);
    for(double wah=0; wah<5; wah+=4.662){
      toneAC(391.995+wah,vol,50,false);
    }
    toneAC(415.305,vol,100,false);
    delay(80);
    for(int j=0; j<7; j++){
      toneAC(391.995,vol,70,false);
      toneAC(415.305,vol,70,false);
    }    
}

void card_problem() {
  toneAC(500,vol,50,false);
  delay(100);
  toneAC(500,vol,50,false);
  blinkLED(CRGB::Red,5,200);
  while (1);
}

void file_problem() {
  blinkLED(CRGB::Yellow, 10, 30);
}

void rtc_failure() {
  blinkLED(CRGB::Green,5,200);
  while (1);
}

void acl_enabled() {
  toneAC(1000,vol,50,false);
  delay(75);
  toneAC(1500,vol,50,false);
  delay(75);
  toneAC(2000,vol,50,false);
}

void good_to_go() {
  delay(200);
  toneAC(1100,vol,200,false);
  delay(100);
  toneAC(1300,vol,200,false);
  if (def_bright > 0) {
    leds[0] = def_color;  
    leds[0] %= def_bright;
    FastLED.show();
  }
}

void entry_logged() {
  toneAC(1400,vol,200,true);
  blinkLED(CRGB::Green,1,100);
}

void same_card() {
  blinkLED(CRGB::Red, 20, 1);
  blinkLED(CRGB::Green, 20, 1);
  blinkLED(CRGB::Red, 20, 1);
  blinkLED(CRGB::Green, 20, 1);
}