**AI-Driven Shoes for the Visually Impaired – Detailed Project Description**

This project is designed to help visually impaired individuals understand their surroundings and move more safely using artificial intelligence, distance sensing, and voice assistance. The system uses three separate microcontroller boards, with each board assigned a specific task. Dividing the work between the boards improves performance, reduces processing load, and makes the overall system easier to test and manage.

The first board is the **ESP32-S3**, which acts as the vision and artificial intelligence unit. A camera module is connected to this board and continuously captures images of the user’s surroundings. These images are analysed using a trained Edge Impulse object-detection model. The AI model identifies important objects such as people, vehicles, chairs, doors, poles, or staircases. It also determines whether the detected object is located on the left, directly ahead, or on the right. Once the object is identified, the ESP32-S3 sends the object name, direction, and confidence level to the first ESP32-WROOM-32 through UART communication.

The first **ESP32-WROOM-32**, referred to as WROOM #1, acts as the central control and decision-making unit. It receives the AI detection result from the ESP32-S3 and simultaneously collects distance measurements from three VL53L1X distance sensors positioned toward the left, centre, and right. These sensors help the system measure how close an obstacle is in each direction.

WROOM #1 combines the AI information with the distance readings. For example, if the ESP32-S3 detects a person ahead, WROOM #1 uses the centre distance sensor to determine how far away the person is. It then decides whether the obstacle requires a normal warning or an urgent danger warning. After processing the information, WROOM #1 sends a complete alert message to the second ESP32-WROOM-32.

The second **ESP32-WROOM-32**, referred to as WROOM #2, handles Bluetooth headset communication and voice interaction. This board receives the final alert from WROOM #1 and converts the structured data into a natural voice response. For example, a message containing a person, an ahead direction, and a distance of 1.5 metres can be converted into the sentence, “A person is ahead, approximately one and a half metres away.”

For full Bluetooth headset support, WROOM #2 uses the Bluetooth Hands-Free Profile, also called HFP. Through HFP, the ESP32 behaves similarly to a smartphone and communicates with both the microphone and speaker of the Bluetooth headset. This allows the system to send voice warnings to the user and also receive voice input from the headset microphone.

The project can initially use a push button to simulate the user asking, “Is there any obstacle?” When the button is pressed, WROOM #2 sends a scan command to WROOM #1. WROOM #1 then requests a fresh camera scan from the ESP32-S3, reads the distance sensors, processes the result, and sends the final information back to WROOM #2. The response is then played through the Bluetooth headset.

In the advanced version, the headset microphone can be used to receive spoken commands such as “What is ahead?”, “Check my left”, or “Is there any obstacle?” These voice commands can be recognised using a fixed-command speech-recognition system. Once the command is identified, WROOM #2 sends the appropriate instruction to WROOM #1 and provides the response through the headset.

The three boards communicate using UART. The ESP32-S3 sends AI results to WROOM #1, while WROOM #1 sends final navigation alerts to WROOM #2. All three boards must share a common ground, and the UART TX and RX pins must be crossed correctly.

The entire system operates continuously. The camera monitors the surroundings, the distance sensors measure nearby obstacles, the central controller combines the data, and the Bluetooth board delivers clear voice guidance. This makes the smart shoes capable of providing real-time obstacle identification, distance estimation, direction detection, and voice-based assistance.

The project can improve the independence and safety of visually impaired users by providing them with immediate information about nearby people, objects, vehicles, and environmental hazards. It also demonstrates the practical use of embedded systems, artificial intelligence, sensor integration, UART communication, Bluetooth audio, and assistive technology in solving a real-world problem.
