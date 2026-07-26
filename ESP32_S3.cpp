/*
BOARD: ESP32-S3 with PSRAM
   JOB OF THIS BOARD:
   1. Capture camera image
   2. Run Edge Impulse object detection
   3. Find object direction
   4. Send result to WROOM-32 #1 through UART

   MESSAGE FORMAT:
   OBJECT=person;DIRECTION=AHEAD;CONFIDENCE=0.91
*/

#include <Arduino.h>
#include "esp_camera.h"

// =====================================================
// REPLACE WITH YOUR EDGE IMPULSE HEADER
// =====================================================

// Example:
// #include <ai_driven_shoes_inferencing.h>

#include <your_project_inferencing.h>

// =====================================================
// UART CONNECTION TO WROOM-32 #1
// =====================================================

// Select two GPIO pins that are NOT used by your camera.
#define WROOM_RX_PIN 1
#define WROOM_TX_PIN 2

#define UART_BAUD_RATE 115200

HardwareSerial ControlBoard(1);

// =====================================================
// CAMERA PIN CONFIGURATION
// =====================================================
//
// IMPORTANT:
// These pins MUST match your exact ESP32-S3 camera board.
//
// Do not copy random camera pins.
// Replace the values below using your board schematic
// or its official CameraWebServer pin configuration.
//

#define CAMERA_PIN_PWDN   -1
#define CAMERA_PIN_RESET  -1

#define CAMERA_PIN_XCLK   -1
#define CAMERA_PIN_SIOD   -1
#define CAMERA_PIN_SIOC   -1

#define CAMERA_PIN_D0     -1
#define CAMERA_PIN_D1     -1
#define CAMERA_PIN_D2     -1
#define CAMERA_PIN_D3     -1
#define CAMERA_PIN_D4     -1
#define CAMERA_PIN_D5     -1
#define CAMERA_PIN_D6     -1
#define CAMERA_PIN_D7     -1

#define CAMERA_PIN_VSYNC  -1
#define CAMERA_PIN_HREF   -1
#define CAMERA_PIN_PCLK   -1

// =====================================================
// SYSTEM SETTINGS
// =====================================================

// Minimum accepted AI confidence
constexpr float CONFIDENCE_THRESHOLD = 0.70F;

// Time between automatic scans
constexpr unsigned long SCAN_INTERVAL_MS = 1000;

// Camera input size must match the Edge Impulse model.
constexpr int MODEL_WIDTH = EI_CLASSIFIER_INPUT_WIDTH;
constexpr int MODEL_HEIGHT = EI_CLASSIFIER_INPUT_HEIGHT;

static uint8_t *imageBuffer = nullptr;

unsigned long previousScanTime = 0;

// =====================================================
// CHECK CAMERA PIN CONFIGURATION
// =====================================================

bool cameraPinsConfigured()
{
    if (CAMERA_PIN_XCLK < 0 ||
        CAMERA_PIN_SIOD < 0 ||
        CAMERA_PIN_SIOC < 0 ||
        CAMERA_PIN_D0 < 0 ||
        CAMERA_PIN_D1 < 0 ||
        CAMERA_PIN_D2 < 0 ||
        CAMERA_PIN_D3 < 0 ||
        CAMERA_PIN_D4 < 0 ||
        CAMERA_PIN_D5 < 0 ||
        CAMERA_PIN_D6 < 0 ||
        CAMERA_PIN_D7 < 0 ||
        CAMERA_PIN_VSYNC < 0 ||
        CAMERA_PIN_HREF < 0 ||
        CAMERA_PIN_PCLK < 0)
    {
        return false;
    }

    return true;
}

// =====================================================
// INITIALISE CAMERA
// =====================================================

bool initialiseCamera()
{
    if (!cameraPinsConfigured())
    {
        Serial.println();
        Serial.println("ERROR: Camera pins are not configured.");
        Serial.println("Enter the correct GPIO values for your board.");
        return false;
    }

    camera_config_t config = {};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;

    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d7 = CAMERA_PIN_D7;

    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;

    config.pin_sccb_sda = CAMERA_PIN_SIOD;
    config.pin_sccb_scl = CAMERA_PIN_SIOC;

    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;

    config.xclk_freq_hz = 20000000;

    /*
       RGB565 is used because it is simpler to convert
       into the RGB888 format required by Edge Impulse.
    */
    config.pixel_format = PIXFORMAT_RGB565;

    /*
       Train/export your Edge Impulse model at 96 × 96
       for this first version.
    */
    config.frame_size = FRAMESIZE_96X96;

    config.jpeg_quality = 12;

    if (psramFound())
    {
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    }
    else
    {
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t cameraError = esp_camera_init(&config);

    if (cameraError != ESP_OK)
    {
        Serial.printf(
            "Camera initialisation failed. Error: 0x%X\n",
            cameraError
        );

        return false;
    }

    sensor_t *cameraSensor = esp_camera_sensor_get();

    if (cameraSensor != nullptr)
    {
        // Optional image adjustments
        cameraSensor->set_brightness(cameraSensor, 0);
        cameraSensor->set_contrast(cameraSensor, 0);
        cameraSensor->set_saturation(cameraSensor, 0);
    }

    Serial.println("Camera initialised successfully.");

    return true;
}

// =====================================================
// CAPTURE CAMERA IMAGE
// =====================================================

bool captureImage()
{
    camera_fb_t *frame = esp_camera_fb_get();

    if (frame == nullptr)
    {
        Serial.println("ERROR: Camera frame capture failed.");
        return false;
    }

    if (frame->format != PIXFORMAT_RGB565)
    {
        Serial.println("ERROR: Camera frame is not RGB565.");
        esp_camera_fb_return(frame);
        return false;
    }

    const size_t expectedPixels =
        static_cast<size_t>(MODEL_WIDTH) *
        static_cast<size_t>(MODEL_HEIGHT);

    const size_t capturedPixels = frame->len / 2;

    if (capturedPixels < expectedPixels)
    {
        Serial.println("ERROR: Camera image is smaller than AI input.");
        esp_camera_fb_return(frame);
        return false;
    }

    /*
       RGB565 uses two bytes per pixel.

       Each pixel is converted into:
       Red, Green and Blue bytes.
    */
    for (size_t pixelIndex = 0;
         pixelIndex < expectedPixels;
         pixelIndex++)
    {
        const size_t framePosition = pixelIndex * 2;
        const size_t imagePosition = pixelIndex * 3;

        const uint16_t pixel =
            static_cast<uint16_t>(
                frame->buf[framePosition] << 8
            ) |
            frame->buf[framePosition + 1];

        const uint8_t red =
            static_cast<uint8_t>(
                ((pixel >> 11) & 0x1F) * 255 / 31
            );

        const uint8_t green =
            static_cast<uint8_t>(
                ((pixel >> 5) & 0x3F) * 255 / 63
            );

        const uint8_t blue =
            static_cast<uint8_t>(
                (pixel & 0x1F) * 255 / 31
            );

        imageBuffer[imagePosition] = red;
        imageBuffer[imagePosition + 1] = green;
        imageBuffer[imagePosition + 2] = blue;
    }

    esp_camera_fb_return(frame);

    return true;
}

// =====================================================
// EDGE IMPULSE IMAGE CALLBACK
// =====================================================

static int provideImageData(
    size_t offset,
    size_t length,
    float *output
)
{
    size_t bufferPosition = offset * 3;

    for (size_t outputPosition = 0;
         outputPosition < length;
         outputPosition++)
    {
        const uint8_t red =
            imageBuffer[bufferPosition];

        const uint8_t green =
            imageBuffer[bufferPosition + 1];

        const uint8_t blue =
            imageBuffer[bufferPosition + 2];

        /*
           Edge Impulse expects each RGB pixel packed
           into one numerical value.
        */
        output[outputPosition] =
            static_cast<float>(
                (red << 16) |
                (green << 8) |
                blue
            );

        bufferPosition += 3;
    }

    return 0;
}

// =====================================================
// DETERMINE DIRECTION
// =====================================================

String determineDirection(
    const int centreX,
    const int imageWidth
)
{
    const int leftBoundary = imageWidth / 3;
    const int rightBoundary = (imageWidth * 2) / 3;

    if (centreX < leftBoundary)
    {
        return "LEFT";
    }

    if (centreX > rightBoundary)
    {
        return "RIGHT";
    }

    return "AHEAD";
}

// =====================================================
// SEND UART MESSAGE
// =====================================================

void sendDetectionMessage(
    const String &objectName,
    const String &direction,
    const float confidence
)
{
    String message;

    message.reserve(100);

    message =
        "OBJECT=" + objectName +
        ";DIRECTION=" + direction +
        ";CONFIDENCE=" + String(confidence, 2);

    ControlBoard.println(message);

    Serial.print("Sent to WROOM-32 #1: ");
    Serial.println(message);
}

void sendClearMessage()
{
    const String message =
        "OBJECT=NONE;DIRECTION=NONE;CONFIDENCE=0.00";

    ControlBoard.println(message);

    Serial.println("Sent to WROOM-32 #1: " + message);
}

// =====================================================
// RUN AI OBJECT DETECTION
// =====================================================

void runObjectDetection()
{
    if (!captureImage())
    {
        ControlBoard.println("ERROR=CAMERA_CAPTURE_FAILED");
        return;
    }

    signal_t signal = {};

    signal.total_length =
        static_cast<size_t>(MODEL_WIDTH) *
        static_cast<size_t>(MODEL_HEIGHT);

    signal.get_data = provideImageData;

    ei_impulse_result_t result = {};

    const EI_IMPULSE_ERROR classifierError =
        run_classifier(&signal, &result, false);

    if (classifierError != EI_IMPULSE_OK)
    {
        Serial.printf(
            "ERROR: Edge Impulse classifier returned %d\n",
            classifierError
        );

        ControlBoard.println("ERROR=AI_CLASSIFIER_FAILED");
        return;
    }

#if EI_CLASSIFIER_OBJECT_DETECTION == 1

    bool objectFound = false;

    String bestObject = "";
    String bestDirection = "NONE";

    float bestConfidence = 0.0F;

    for (size_t index = 0;
         index < EI_CLASSIFIER_OBJECT_DETECTION_COUNT;
         index++)
    {
        const ei_impulse_result_bounding_box_t detection =
            result.bounding_boxes[index];

        if (detection.value < CONFIDENCE_THRESHOLD)
        {
            continue;
        }

        if (detection.width == 0 ||
            detection.height == 0)
        {
            continue;
        }

        Serial.print("Detected: ");
        Serial.print(detection.label);

        Serial.print(" | Confidence: ");
        Serial.print(detection.value, 2);

        Serial.print(" | X: ");
        Serial.print(detection.x);

        Serial.print(" | Y: ");
        Serial.println(detection.y);

        if (detection.value > bestConfidence)
        {
            bestConfidence = detection.value;
            bestObject = String(detection.label);

            const int objectCentreX =
                static_cast<int>(detection.x) +
                static_cast<int>(detection.width / 2);

            bestDirection = determineDirection(
                objectCentreX,
                MODEL_WIDTH
            );

            objectFound = true;
        }
    }

    if (objectFound)
    {
        bestObject.toUpperCase();

        sendDetectionMessage(
            bestObject,
            bestDirection,
            bestConfidence
        );
    }
    else
    {
        sendClearMessage();
    }

#else

    Serial.println(
        "ERROR: The installed Edge Impulse model is "
        "not an object-detection model."
    );

    ControlBoard.println(
        "ERROR=MODEL_IS_NOT_OBJECT_DETECTION"
    );

#endif
}

// =====================================================
// CHECK COMMANDS FROM WROOM-32 #1
// =====================================================

void checkIncomingCommand()
{
    if (!ControlBoard.available())
    {
        return;
    }

    String command =
        ControlBoard.readStringUntil('\n');

    command.trim();
    command.toUpperCase();

    Serial.print("Command received: ");
    Serial.println(command);

    if (command == "SCAN" ||
        command == "SCAN_AHEAD" ||
        command == "WHAT_IS_AHEAD")
    {
        runObjectDetection();
    }
    else if (command == "PING")
    {
        ControlBoard.println("STATUS=S3_READY");
    }
    else
    {
        ControlBoard.println(
            "ERROR=UNKNOWN_COMMAND"
        );
    }
}

// =====================================================
// ALLOCATE IMAGE MEMORY
// =====================================================

bool allocateImageBuffer()
{
    const size_t requiredBytes =
        static_cast<size_t>(MODEL_WIDTH) *
        static_cast<size_t>(MODEL_HEIGHT) *
        3;

    if (psramFound())
    {
        imageBuffer =
            static_cast<uint8_t *>(
                ps_malloc(requiredBytes)
            );
    }
    else
    {
        imageBuffer =
            static_cast<uint8_t *>(
                malloc(requiredBytes)
            );
    }

    if (imageBuffer == nullptr)
    {
        Serial.printf(
            "ERROR: Could not allocate %u bytes.\n",
            static_cast<unsigned int>(requiredBytes)
        );

        return false;
    }

    Serial.printf(
        "Image buffer allocated: %u bytes.\n",
        static_cast<unsigned int>(requiredBytes)
    );

    return true;
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
    Serial.println("AI DRIVEN SHOES");
    Serial.println("ESP32-S3 CAMERA AND AI BOARD");
    Serial.println("================================");

    ControlBoard.begin(
        UART_BAUD_RATE,
        SERIAL_8N1,
        WROOM_RX_PIN,
        WROOM_TX_PIN
    );

    ControlBoard.setTimeout(100);

    Serial.printf(
        "AI input size: %d x %d\n",
        MODEL_WIDTH,
        MODEL_HEIGHT
    );

    if (!psramFound())
    {
        Serial.println(
            "WARNING: PSRAM was not detected."
        );
    }
    else
    {
        Serial.println("PSRAM detected.");
    }

    if (MODEL_WIDTH != 96 ||
        MODEL_HEIGHT != 96)
    {
        Serial.println();
        Serial.println(
            "ERROR: This first program expects a "
            "96 x 96 Edge Impulse model."
        );

        Serial.println(
            "Retrain/export the model at 96 x 96 "
            "or add image resizing."
        );

        while (true)
        {
            delay(1000);
        }
    }

    if (!allocateImageBuffer())
    {
        while (true)
        {
            delay(1000);
        }
    }

    if (!initialiseCamera())
    {
        while (true)
        {
            delay(1000);
        }
    }

    ControlBoard.println("STATUS=S3_READY");

    Serial.println();
    Serial.println("ESP32-S3 is ready.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
    // Respond to commands from WROOM-32 #1
    checkIncomingCommand();

    // Automatic environmental scan
    if (millis() - previousScanTime >= SCAN_INTERVAL_MS)
    {
        previousScanTime = millis();

        runObjectDetection();
    }

    delay(10);
}