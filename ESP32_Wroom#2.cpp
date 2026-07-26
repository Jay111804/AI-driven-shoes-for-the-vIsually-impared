/*BOARD:
   ESP32-WROOM-32 #2

   RESPONSIBILITIES:
   1. Receive navigation alerts from WROOM-32 #1
   2. Convert structured data into natural sentences
   3. Request a scan when the user asks a question
   4. Control Bluetooth voice playback
   5. Later receive speech through Bluetooth HFP

   MESSAGE RECEIVED:
   ALERT=PERSON;DIRECTION=AHEAD;DISTANCE=1450;
   CONFIDENCE=0.91;PRIORITY=NORMAL

   COMMAND SENT:
   IS_THERE_AN_OBSTACLE
*/

#include <Arduino.h>

// =====================================================
// UART CONNECTION TO WROOM-32 #1
// =====================================================

// WROOM #1 GPIO 27 TX connects to this RX pin.
// WROOM #1 GPIO 26 RX connects to this TX pin.

#define CONTROL_RX_PIN 16
#define CONTROL_TX_PIN 17

HardwareSerial ControlBoardSerial(2);

// =====================================================
// USER INPUT
// =====================================================

// Temporary push button used to simulate the question:
// "Is there any obstacle?"

#define ASK_BUTTON_PIN 25

// Optional acknowledgement LED
#define STATUS_LED_PIN 2

// =====================================================
// SYSTEM SETTINGS
// =====================================================

constexpr uint32_t UART_BAUD_RATE = 115200;

constexpr unsigned long BUTTON_DEBOUNCE_MS = 250;
constexpr unsigned long REPEAT_ALERT_DELAY_MS = 2000;
constexpr unsigned long RESPONSE_TIMEOUT_MS = 5000;

// =====================================================
// VARIABLES
// =====================================================

String controlInputBuffer = "";
String previousSpokenMessage = "";

unsigned long previousButtonTime = 0;
unsigned long previousAlertTime = 0;
unsigned long scanRequestedAt = 0;

bool waitingForScanResponse = false;

// =====================================================
// EXTRACT A FIELD FROM A MESSAGE
// =====================================================

String getMessageField(
    const String &message,
    const String &fieldName
)
{
    const String searchText = fieldName + "=";

    int startPosition = message.indexOf(searchText);

    if (startPosition < 0)
    {
        return "";
    }

    startPosition += searchText.length();

    int endPosition =
        message.indexOf(';', startPosition);

    if (endPosition < 0)
    {
        endPosition = message.length();
    }

    return message.substring(
        startPosition,
        endPosition
    );
}

// =====================================================
// CONVERT DIRECTION INTO NATURAL SPEECH
// =====================================================

String directionToSpeech(String direction)
{
    direction.toUpperCase();

    if (direction == "LEFT")
    {
        return "on your left";
    }

    if (direction == "RIGHT")
    {
        return "on your right";
    }

    if (direction == "AHEAD")
    {
        return "ahead";
    }

    return "nearby";
}

// =====================================================
// CONVERT OBJECT NAME INTO NATURAL SPEECH
// =====================================================

String objectToSpeech(String objectName)
{
    objectName.toUpperCase();

    if (objectName == "PERSON")
    {
        return "a person";
    }

    if (objectName == "VEHICLE" ||
        objectName == "CAR")
    {
        return "a vehicle";
    }

    if (objectName == "STAIRCASE" ||
        objectName == "STAIRS")
    {
        return "a staircase";
    }

    if (objectName == "CHAIR")
    {
        return "a chair";
    }

    if (objectName == "DOOR")
    {
        return "a door";
    }

    if (objectName == "POLE")
    {
        return "a pole";
    }

    if (objectName == "UNKNOWN_OBSTACLE")
    {
        return "an obstacle";
    }

    objectName.toLowerCase();

    return "a " + objectName;
}

// =====================================================
// CONVERT DISTANCE INTO SPEECH
// =====================================================

String distanceToSpeech(const String &distanceText)
{
    if (distanceText.length() == 0 ||
        distanceText == "UNKNOWN")
    {
        return "at an unknown distance";
    }

    const long distanceMillimetres =
        distanceText.toInt();

    if (distanceMillimetres <= 0)
    {
        return "";
    }

    if (distanceMillimetres < 1000)
    {
        const int centimetres =
            distanceMillimetres / 10;

        return String(centimetres) +
               " centimetres away";
    }

    const float metres =
        distanceMillimetres / 1000.0F;

    return String(metres, 1) +
           " metres away";
}

// =====================================================
// BLUETOOTH VOICE OUTPUT PLACEHOLDER
// =====================================================

void speakMessage(const String &sentence)
{
    /*
       This currently prints the sentence.

       The final HFP Bluetooth audio function will
       replace this section.

       Examples:
       playRecordedAlert(sentence);
       sendPCMToBluetoothHeadset(sentence);
    */

    digitalWrite(STATUS_LED_PIN, HIGH);

    Serial.println();
    Serial.println("VOICE OUTPUT:");
    Serial.println(sentence);
    Serial.println();

    delay(100);

    digitalWrite(STATUS_LED_PIN, LOW);
}

// =====================================================
// PREVENT UNNECESSARY REPEATED WARNINGS
// =====================================================

void speakIfRequired(
    const String &sentence,
    const bool forceSpeech = false
)
{
    const unsigned long currentTime = millis();

    const bool messageChanged =
        sentence != previousSpokenMessage;

    const bool repeatDelayCompleted =
        currentTime - previousAlertTime >=
        REPEAT_ALERT_DELAY_MS;

    if (!forceSpeech &&
        !messageChanged &&
        !repeatDelayCompleted)
    {
        return;
    }

    speakMessage(sentence);

    previousSpokenMessage = sentence;
    previousAlertTime = currentTime;
}

// =====================================================
// CREATE THE SPOKEN NAVIGATION SENTENCE
// =====================================================

String buildNavigationSentence(
    String objectName,
    String direction,
    String distanceText,
    String priority
)
{
    objectName.toUpperCase();
    priority.toUpperCase();

    if (objectName == "CLEAR" ||
        objectName == "NONE")
    {
        return "The path appears clear. "
               "No nearby obstacle was detected.";
    }

    const String spokenObject =
        objectToSpeech(objectName);

    const String spokenDirection =
        directionToSpeech(direction);

    const String spokenDistance =
        distanceToSpeech(distanceText);

    String sentence;

    if (priority == "DANGER")
    {
        sentence = "Warning. ";
    }

    sentence += spokenObject;
    sentence += " is ";
    sentence += spokenDirection;

    if (spokenDistance.length() > 0)
    {
        sentence += ", ";
        sentence += spokenDistance;
    }

    sentence += ".";

    return sentence;
}

// =====================================================
// PROCESS ALERT FROM WROOM-32 #1
// =====================================================

void processAlertMessage(const String &message)
{
    String objectName =
        getMessageField(message, "ALERT");

    String direction =
        getMessageField(message, "DIRECTION");

    String distance =
        getMessageField(message, "DISTANCE");

    String priority =
        getMessageField(message, "PRIORITY");

    if (objectName.length() == 0)
    {
        Serial.println(
            "ERROR: Alert field is missing."
        );

        return;
    }

    if (direction.length() == 0)
    {
        direction = "NONE";
    }

    if (priority.length() == 0)
    {
        priority = "NORMAL";
    }

    String sentence =
        buildNavigationSentence(
            objectName,
            direction,
            distance,
            priority
        );

    /*
       Always speak the answer when the user has
       specifically requested a scan.
    */
    const bool forceResponse =
        waitingForScanResponse;

    waitingForScanResponse = false;

    speakIfRequired(
        sentence,
        forceResponse
    );
}

// =====================================================
// PROCESS MULTIPLE DISTANCE RESULTS
// =====================================================

void processDistanceMessage(
    const String &message
)
{
    String leftDistance =
        getMessageField(message, "LEFT");

    String aheadDistance =
        getMessageField(message, "AHEAD");

    String rightDistance =
        getMessageField(message, "RIGHT");

    String sentence =
        "Distance check. Left, " +
        distanceToSpeech(leftDistance) +
        ". Ahead, " +
        distanceToSpeech(aheadDistance) +
        ". Right, " +
        distanceToSpeech(rightDistance) +
        ".";

    waitingForScanResponse = false;

    speakIfRequired(sentence, true);
}

// =====================================================
// PROCESS SYSTEM STATUS
// =====================================================

void processStatusMessage(
    const String &message
)
{
    String status =
        getMessageField(message, "STATUS");

    Serial.print("Control-board status: ");
    Serial.println(status);

    if (status == "CONTROL_BOARD_READY")
    {
        speakIfRequired(
            "Navigation system is ready.",
            true
        );
    }
}

// =====================================================
// PROCESS SYSTEM ERRORS
// =====================================================

void processErrorMessage(
    const String &message
)
{
    String errorText = message;

    errorText.replace("_", " ");
    errorText.toLowerCase();

    speakIfRequired(
        "System warning. " + errorText,
        true
    );
}

// =====================================================
// PROCESS COMPLETE UART MESSAGE
// =====================================================

void processControlMessage(String message)
{
    message.trim();

    if (message.length() == 0)
    {
        return;
    }

    Serial.print("Received from WROOM #1: ");
    Serial.println(message);

    if (message.startsWith("ALERT="))
    {
        processAlertMessage(message);
    }
    else if (message.startsWith("DISTANCES;"))
    {
        processDistanceMessage(message);
    }
    else if (message.startsWith("STATUS="))
    {
        processStatusMessage(message);
    }
    else if (message.startsWith("ERROR=") ||
             message.startsWith("SYSTEM_ERROR="))
    {
        processErrorMessage(message);
    }
    else
    {
        Serial.println(
            "WARNING: Unknown message received."
        );
    }
}

// =====================================================
// READ COMPLETE UART LINES
// =====================================================

void readControlBoardSerial()
{
    while (ControlBoardSerial.available())
    {
        const char receivedCharacter =
            static_cast<char>(
                ControlBoardSerial.read()
            );

        if (receivedCharacter == '\n')
        {
            processControlMessage(
                controlInputBuffer
            );

            controlInputBuffer = "";
        }
        else if (receivedCharacter != '\r')
        {
            if (controlInputBuffer.length() < 250)
            {
                controlInputBuffer +=
                    receivedCharacter;
            }
            else
            {
                Serial.println(
                    "ERROR: UART message too long."
                );

                controlInputBuffer = "";
            }
        }
    }
}

// =====================================================
// REQUEST ENVIRONMENT SCAN
// =====================================================

void requestObstacleScan()
{
    Serial.println();
    Serial.println(
        "User request: Is there any obstacle?"
    );

    ControlBoardSerial.println(
        "IS_THERE_AN_OBSTACLE"
    );

    waitingForScanResponse = true;
    scanRequestedAt = millis();

    speakMessage(
        "Checking the surroundings."
    );
}

// =====================================================
// REQUEST DISTANCE INFORMATION
// =====================================================

void requestDistanceCheck()
{
    ControlBoardSerial.println("DISTANCE");

    waitingForScanResponse = true;
    scanRequestedAt = millis();

    speakMessage(
        "Checking obstacle distances."
    );
}

// =====================================================
// CHECK THE TEMPORARY BUTTON
// =====================================================

void checkAskButton()
{
    const bool buttonPressed =
        digitalRead(ASK_BUTTON_PIN) == LOW;

    const unsigned long currentTime = millis();

    if (buttonPressed &&
        currentTime - previousButtonTime >=
        BUTTON_DEBOUNCE_MS)
    {
        previousButtonTime = currentTime;

        requestObstacleScan();
    }
}

// =====================================================
// CHECK RESPONSE TIMEOUT
// =====================================================

void checkResponseTimeout()
{
    if (!waitingForScanResponse)
    {
        return;
    }

    if (millis() - scanRequestedAt >=
        RESPONSE_TIMEOUT_MS)
    {
        waitingForScanResponse = false;

        speakIfRequired(
            "I could not receive the sensor response.",
            true
        );
    }
}

// =====================================================
// TEST COMMANDS THROUGH SERIAL MONITOR
// =====================================================

void checkSerialMonitor()
{
    if (!Serial.available())
    {
        return;
    }

    String command =
        Serial.readStringUntil('\n');

    command.trim();
    command.toUpperCase();

    if (command == "SCAN")
    {
        requestObstacleScan();
    }
    else if (command == "DISTANCE")
    {
        requestDistanceCheck();
    }
    else if (command == "PING")
    {
        ControlBoardSerial.println("PING");
    }
    else
    {
        Serial.println(
            "Commands: SCAN, DISTANCE, PING"
        );
    }
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);

    delay(1500);

    Serial.println();
    Serial.println("================================");
    Serial.println("AI-DRIVEN SHOES");
    Serial.println("WROOM-32 #2 VOICE CONTROLLER");
    Serial.println("================================");

    pinMode(
        ASK_BUTTON_PIN,
        INPUT_PULLUP
    );

    pinMode(
        STATUS_LED_PIN,
        OUTPUT
    );

    digitalWrite(
        STATUS_LED_PIN,
        LOW
    );

    ControlBoardSerial.begin(
        UART_BAUD_RATE,
        SERIAL_8N1,
        CONTROL_RX_PIN,
        CONTROL_TX_PIN
    );

    ControlBoardSerial.setTimeout(100);

    Serial.println(
        "Voice controller initialised."
    );

    Serial.println(
        "Press the button or enter SCAN "
        "in Serial Monitor."
    );

    ControlBoardSerial.println("PING");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
    readControlBoardSerial();

    checkAskButton();

    checkSerialMonitor();

    checkResponseTimeout();

    delay(5);
}