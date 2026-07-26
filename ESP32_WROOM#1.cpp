/*
   PROJECT: AI-Driven Shoes for the Visually Impaired

   BOARD: ESP32-WROOM-32 #1

   RESPONSIBILITIES:
   1. Receive AI detection from ESP32-S3
   2. Read left, centre and right distance sensors
   3. Combine object, direction and distance data
   4. Decide whether an alert is required
   5. Send the final message to ESP32-WROOM-32 #2
   6. Forward scan requests from WROOM-32 #2 to ESP32-S3

   S3 MESSAGE EXAMPLE:
   OBJECT=PERSON;DIRECTION=AHEAD;CONFIDENCE=0.91

   MESSAGE TO WROOM #2:
   ALERT=PERSON;DIRECTION=AHEAD;DISTANCE=1450;CONFIDENCE=0.91
*/

#include <Arduino.h>
#include <Wire.h>
#include <VL53L1X.h>

// =====================================================
// I2C PINS
// =====================================================

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// =====================================================
// VL53L1X XSHUT PINS
// =====================================================

#define LEFT_XSHUT_PIN    25
#define CENTRE_XSHUT_PIN  32
#define RIGHT_XSHUT_PIN   33

// Each sensor must have a different I2C address.
#define LEFT_SENSOR_ADDRESS    0x30
#define CENTRE_SENSOR_ADDRESS  0x31
#define RIGHT_SENSOR_ADDRESS   0x32

VL53L1X leftSensor;
VL53L1X centreSensor;
VL53L1X rightSensor;

// =====================================================
// UART: WROOM #1 ↔ ESP32-S3
// =====================================================

// S3 TX connects to GPIO 16.
// S3 RX connects to GPIO 17.

#define S3_RX_PIN 16
#define S3_TX_PIN 17

HardwareSerial S3Serial(2);

// =====================================================
// UART: WROOM #1 ↔ WROOM #2
// =====================================================

// WROOM #2 TX connects to GPIO 26.
// WROOM #2 RX connects to GPIO 27.

#define VOICE_RX_PIN 26
#define VOICE_TX_PIN 27

HardwareSerial VoiceBoardSerial(1);

// =====================================================
// SYSTEM SETTINGS
// =====================================================

constexpr uint32_t UART_BAUD_RATE = 115200;

// Send a warning when the obstacle is within 2.5 metres.
constexpr uint16_t WARNING_DISTANCE_MM = 2500;

// High-priority warning below 80 centimetres.
constexpr uint16_t DANGER_DISTANCE_MM = 800;

// Read the distance sensors every 100 ms.
constexpr unsigned long SENSOR_INTERVAL_MS = 100;

// Request an automatic AI scan every one second.
constexpr unsigned long AI_SCAN_INTERVAL_MS = 1000;

// Prevent identical voice messages from repeating too quickly.
constexpr unsigned long ALERT_COOLDOWN_MS = 1500;

// AI detection confidence limit.
constexpr float MINIMUM_CONFIDENCE = 0.70F;

// =====================================================
// AI RESULT STRUCTURE
// =====================================================

struct AIResult
{
    String object;
    String direction;
    float confidence;
    bool valid;
    unsigned long receivedAt;
};

AIResult latestAIResult = {
    "NONE",
    "NONE",
    0.0F,
    false,
    0
};

// =====================================================
// DISTANCE DATA
// =====================================================

uint16_t leftDistance = 0;
uint16_t centreDistance = 0;
uint16_t rightDistance = 0;

unsigned long previousSensorRead = 0;
unsigned long previousAIScan = 0;
unsigned long previousAlertTime = 0;

String previousAlert = "";

// UART receiving buffers
String s3InputBuffer = "";
String voiceInputBuffer = "";

// =====================================================
// READ A FIELD FROM A STRUCTURED MESSAGE
// =====================================================

String getMessageField(
    const String &message,
    const String &fieldName
)
{
    String searchText = fieldName + "=";

    int startPosition = message.indexOf(searchText);

    if (startPosition < 0)
    {
        return "";
    }

    startPosition += searchText.length();

    int endPosition = message.indexOf(';', startPosition);

    if (endPosition < 0)
    {
        endPosition = message.length();
    }

    return message.substring(startPosition, endPosition);
}

// =====================================================
// INITIALISE ONE VL53L1X SENSOR
// =====================================================

bool initialiseOneSensor(
    VL53L1X &sensor,
    uint8_t xshutPin,
    uint8_t newAddress,
    const char *sensorName
)
{
    digitalWrite(xshutPin, HIGH);
    delay(20);

    sensor.setTimeout(100);

    if (!sensor.init())
    {
        Serial.print("ERROR: ");
        Serial.print(sensorName);
        Serial.println(" sensor was not detected.");

        return false;
    }

    sensor.setAddress(newAddress);
    sensor.setDistanceMode(VL53L1X::Long);
    sensor.setMeasurementTimingBudget(50000);
    sensor.startContinuous(50);

    Serial.print(sensorName);
    Serial.print(" sensor ready at address 0x");
    Serial.println(newAddress, HEX);

    return true;
}

// =====================================================
// INITIALISE ALL DISTANCE SENSORS
// =====================================================

bool initialiseDistanceSensors()
{
    pinMode(LEFT_XSHUT_PIN, OUTPUT);
    pinMode(CENTRE_XSHUT_PIN, OUTPUT);
    pinMode(RIGHT_XSHUT_PIN, OUTPUT);

    // Shut down all sensors first.
    digitalWrite(LEFT_XSHUT_PIN, LOW);
    digitalWrite(CENTRE_XSHUT_PIN, LOW);
    digitalWrite(RIGHT_XSHUT_PIN, LOW);

    delay(20);

    bool leftReady = initialiseOneSensor(
        leftSensor,
        LEFT_XSHUT_PIN,
        LEFT_SENSOR_ADDRESS,
        "Left"
    );

    bool centreReady = initialiseOneSensor(
        centreSensor,
        CENTRE_XSHUT_PIN,
        CENTRE_SENSOR_ADDRESS,
        "Centre"
    );

    bool rightReady = initialiseOneSensor(
        rightSensor,
        RIGHT_XSHUT_PIN,
        RIGHT_SENSOR_ADDRESS,
        "Right"
    );

    return leftReady && centreReady && rightReady;
}

// =====================================================
// READ ONE SENSOR SAFELY
// =====================================================

uint16_t readOneSensor(
    VL53L1X &sensor,
    const char *sensorName
)
{
    uint16_t distance = sensor.read();

    if (sensor.timeoutOccurred())
    {
        Serial.print("WARNING: ");
        Serial.print(sensorName);
        Serial.println(" sensor timeout.");

        return 0;
    }

    return distance;
}

// =====================================================
// READ ALL DISTANCE SENSORS
// =====================================================

void readDistanceSensors()
{
    leftDistance = readOneSensor(
        leftSensor,
        "Left"
    );

    centreDistance = readOneSensor(
        centreSensor,
        "Centre"
    );

    rightDistance = readOneSensor(
        rightSensor,
        "Right"
    );

    Serial.print("Left: ");
    Serial.print(leftDistance);

    Serial.print(" mm | Centre: ");
    Serial.print(centreDistance);

    Serial.print(" mm | Right: ");
    Serial.print(rightDistance);

    Serial.println(" mm");
}

// =====================================================
// FIND THE DISTANCE FOR AN AI DIRECTION
// =====================================================

uint16_t getDistanceForDirection(
    const String &direction
)
{
    if (direction == "LEFT")
    {
        return leftDistance;
    }

    if (direction == "RIGHT")
    {
        return rightDistance;
    }

    // AHEAD or unknown direction uses centre sensor.
    return centreDistance;
}

// =====================================================
// FIND THE NEAREST SENSOR
// =====================================================

uint16_t getNearestDistance()
{
    uint16_t nearest = 0;

    uint16_t distances[3] = {
        leftDistance,
        centreDistance,
        rightDistance
    };

    for (uint8_t index = 0; index < 3; index++)
    {
        uint16_t distance = distances[index];

        if (distance == 0)
        {
            continue;
        }

        if (nearest == 0 || distance < nearest)
        {
            nearest = distance;
        }
    }

    return nearest;
}

// =====================================================
// FIND DIRECTION OF NEAREST SENSOR
// =====================================================

String getNearestDirection()
{
    uint16_t nearest = getNearestDistance();

    if (nearest == 0)
    {
        return "NONE";
    }

    if (nearest == leftDistance)
    {
        return "LEFT";
    }

    if (nearest == rightDistance)
    {
        return "RIGHT";
    }

    return "AHEAD";
}

// =====================================================
// SEND MESSAGE TO WROOM #2
// =====================================================

void sendToVoiceBoard(
    const String &message,
    bool forceSend = false
)
{
    unsigned long currentTime = millis();

    bool messageChanged = message != previousAlert;

    bool cooldownCompleted =
        currentTime - previousAlertTime >= ALERT_COOLDOWN_MS;

    if (!forceSend &&
        !messageChanged &&
        !cooldownCompleted)
    {
        return;
    }

    VoiceBoardSerial.println(message);

    Serial.print("Sent to WROOM #2: ");
    Serial.println(message);

    previousAlert = message;
    previousAlertTime = currentTime;
}

// =====================================================
// BUILD FINAL NAVIGATION RESULT
// =====================================================

void processNavigationData(
    bool forceResponse = false
)
{
    /*
       Case 1:
       AI successfully identified an object.
    */

    if (latestAIResult.valid &&
        latestAIResult.object != "NONE" &&
        latestAIResult.confidence >= MINIMUM_CONFIDENCE)
    {
        uint16_t objectDistance =
            getDistanceForDirection(
                latestAIResult.direction
            );

        if (objectDistance == 0)
        {
            String message =
                "ALERT=" + latestAIResult.object +
                ";DIRECTION=" + latestAIResult.direction +
                ";DISTANCE=UNKNOWN" +
                ";CONFIDENCE=" +
                String(latestAIResult.confidence, 2);

            sendToVoiceBoard(message, forceResponse);

            return;
        }

        if (objectDistance <= WARNING_DISTANCE_MM ||
            forceResponse)
        {
            String priority = "NORMAL";

            if (objectDistance <= DANGER_DISTANCE_MM)
            {
                priority = "DANGER";
            }

            String message =
                "ALERT=" + latestAIResult.object +
                ";DIRECTION=" + latestAIResult.direction +
                ";DISTANCE=" + String(objectDistance) +
                ";CONFIDENCE=" +
                String(latestAIResult.confidence, 2) +
                ";PRIORITY=" + priority;

            sendToVoiceBoard(message, forceResponse);
        }
        else if (forceResponse)
        {
            sendToVoiceBoard(
                "ALERT=CLEAR;DIRECTION=" +
                latestAIResult.direction +
                ";DISTANCE=" +
                String(objectDistance),
                true
            );
        }

        return;
    }

    /*
       Case 2:
       AI did not identify the object, but a distance
       sensor detected something nearby.
    */

    uint16_t nearestDistance = getNearestDistance();

    if (nearestDistance > 0 &&
        nearestDistance <= WARNING_DISTANCE_MM)
    {
        String nearestDirection =
            getNearestDirection();

        String priority = "NORMAL";

        if (nearestDistance <= DANGER_DISTANCE_MM)
        {
            priority = "DANGER";
        }

        String message =
            "ALERT=UNKNOWN_OBSTACLE" +
            String(";DIRECTION=") +
            nearestDirection +
            ";DISTANCE=" +
            String(nearestDistance) +
            ";CONFIDENCE=0.00" +
            ";PRIORITY=" +
            priority;

        sendToVoiceBoard(message, forceResponse);

        return;
    }

    /*
       Case 3:
       No nearby object was detected.
    */

    if (forceResponse)
    {
        sendToVoiceBoard(
            "ALERT=CLEAR;DIRECTION=NONE;DISTANCE=0",
            true
        );
    }
}

// =====================================================
// PROCESS MESSAGE FROM ESP32-S3
// =====================================================

void processS3Message(String message)
{
    message.trim();

    if (message.length() == 0)
    {
        return;
    }

    Serial.print("Received from S3: ");
    Serial.println(message);

    if (message.startsWith("ERROR="))
    {
        sendToVoiceBoard(
            "SYSTEM_ERROR=S3_" + message.substring(6),
            true
        );

        return;
    }

    if (message.startsWith("STATUS="))
    {
        Serial.println("S3 status received.");
        return;
    }

    String object =
        getMessageField(message, "OBJECT");

    String direction =
        getMessageField(message, "DIRECTION");

    String confidenceText =
        getMessageField(message, "CONFIDENCE");

    if (object.length() == 0 ||
        direction.length() == 0)
    {
        Serial.println(
            "ERROR: Invalid S3 message format."
        );

        return;
    }

    object.toUpperCase();
    direction.toUpperCase();

    latestAIResult.object = object;
    latestAIResult.direction = direction;
    latestAIResult.confidence =
        confidenceText.toFloat();

    latestAIResult.valid =
        object != "NONE";

    latestAIResult.receivedAt = millis();

    processNavigationData();
}

// =====================================================
// READ COMPLETE LINES FROM ESP32-S3
// =====================================================

void readS3Serial()
{
    while (S3Serial.available())
    {
        char receivedCharacter =
            static_cast<char>(S3Serial.read());

        if (receivedCharacter == '\n')
        {
            processS3Message(s3InputBuffer);
            s3InputBuffer = "";
        }
        else if (receivedCharacter != '\r')
        {
            if (s3InputBuffer.length() < 200)
            {
                s3InputBuffer += receivedCharacter;
            }
            else
            {
                s3InputBuffer = "";
            }
        }
    }
}

// =====================================================
// PROCESS COMMAND FROM WROOM #2
// =====================================================

void processVoiceBoardCommand(String command)
{
    command.trim();
    command.toUpperCase();

    if (command.length() == 0)
    {
        return;
    }

    Serial.print("Received from WROOM #2: ");
    Serial.println(command);

    if (command == "SCAN" ||
        command == "WHAT_IS_AHEAD" ||
        command == "IS_THERE_AN_OBSTACLE")
    {
        /*
           Ask the ESP32-S3 to capture and analyse
           a new camera image.
        */
        S3Serial.println("SCAN");

        /*
           Give an immediate distance-only response.
           The AI result will follow after processing.
        */
        readDistanceSensors();
        processNavigationData(true);
    }
    else if (command == "DISTANCE")
    {
        readDistanceSensors();

        String message =
            "DISTANCES;LEFT=" +
            String(leftDistance) +
            ";AHEAD=" +
            String(centreDistance) +
            ";RIGHT=" +
            String(rightDistance);

        sendToVoiceBoard(message, true);
    }
    else if (command == "PING")
    {
        sendToVoiceBoard(
            "STATUS=CONTROL_BOARD_READY",
            true
        );
    }
    else
    {
        sendToVoiceBoard(
            "ERROR=UNKNOWN_COMMAND",
            true
        );
    }
}

// =====================================================
// READ COMPLETE LINES FROM WROOM #2
// =====================================================

void readVoiceBoardSerial()
{
    while (VoiceBoardSerial.available())
    {
        char receivedCharacter =
            static_cast<char>(
                VoiceBoardSerial.read()
            );

        if (receivedCharacter == '\n')
        {
            processVoiceBoardCommand(
                voiceInputBuffer
            );

            voiceInputBuffer = "";
        }
        else if (receivedCharacter != '\r')
        {
            if (voiceInputBuffer.length() < 200)
            {
                voiceInputBuffer +=
                    receivedCharacter;
            }
            else
            {
                voiceInputBuffer = "";
            }
        }
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
    Serial.println("WROOM-32 #1 CENTRAL CONTROLLER");
    Serial.println("================================");

    Wire.begin(
        I2C_SDA_PIN,
        I2C_SCL_PIN
    );

    Wire.setClock(400000);

    // Communication with ESP32-S3
    S3Serial.begin(
        UART_BAUD_RATE,
        SERIAL_8N1,
        S3_RX_PIN,
        S3_TX_PIN
    );

    // Communication with WROOM-32 #2
    VoiceBoardSerial.begin(
        UART_BAUD_RATE,
        SERIAL_8N1,
        VOICE_RX_PIN,
        VOICE_TX_PIN
    );

    if (!initialiseDistanceSensors())
    {
        Serial.println(
            "ERROR: One or more distance sensors failed."
        );

        VoiceBoardSerial.println(
            "SYSTEM_ERROR=DISTANCE_SENSOR_FAILURE"
        );
    }
    else
    {
        Serial.println(
            "All distance sensors are ready."
        );
    }

    S3Serial.println("PING");

    VoiceBoardSerial.println(
        "STATUS=CONTROL_BOARD_READY"
    );

    previousSensorRead = millis();
    previousAIScan = millis();

    Serial.println("Central controller ready.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
    readS3Serial();
    readVoiceBoardSerial();

    unsigned long currentTime = millis();

    // Periodically read all distance sensors.
    if (currentTime - previousSensorRead >=
        SENSOR_INTERVAL_MS)
    {
        previousSensorRead = currentTime;

        readDistanceSensors();
        processNavigationData();
    }

    // Request a new AI camera scan every second.
    if (currentTime - previousAIScan >=
        AI_SCAN_INTERVAL_MS)
    {
        previousAIScan = currentTime;

        S3Serial.println("SCAN");
    }

    delay(5);
}