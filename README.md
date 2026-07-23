# LumaLink

LumaLink is a real-time optical emergency communication system built on **QNX RTOS**. It uses a Raspberry Pi Camera Module 3 Wide to detect high-speed Morse code light flashes, decodes them into text, and uses a locally hosted AI model to generate an operator-friendly emergency summary.

The system demonstrates how deterministic scheduling in QNX enables reliable frame capture for time-critical optical communication, where missing even a few frames can result in an incorrect decoded message.

---

## Hardware

### Receiver
- Raspberry Pi 5
- Raspberry Pi Camera Module 3 Wide
- QNX RTOS

### Transmitter
- Any computer capable of running the Python transmitter script
- Display/monitor used as the light source

---

## Project Structure

```
camera_example1_callback.c      # Camera receiver (QNX/C)
transmitter.py                  # Morse code transmitter
lumalink_ai_server.py           # AI + web dashboard
morse_output/output.txt         # Generated Morse output
```

---

## How It Works

1. The transmitter converts a text message into Morse code.
2. The message is displayed as rapid flashes on another computer's screen.
3. The Raspberry Pi camera captures the flashes.
4. The QNX receiver converts the flashes into Morse code.
5. The Morse code is decoded into English.
6. A local Qwen AI model generates an operator-friendly summary.
7. If an SOS message is detected, a local emergency dashboard is launched.

---

# Running the Receiver

## 1. Compile

From the directory containing the receiver source:

```bash
qcc -Vgcc_ntoaarch64le camera_example1_callback.c \
    -o camera_example1_callback \
    -lcamapi
```

---

## 2. Copy to the Raspberry Pi

```bash
scp camera_example1_callback \
qnxuser@10.113.69.106:/data/home/qnxuser/
```

---

## 3. SSH or TigerVNC into the Raspberry Pi

Make the binary executable:

```bash
chmod +x /data/home/qnxuser/camera_example1_callback
```

---

## 4. Run the receiver

```bash
./camera_example1_callback -u 1
```

---

## 5. View the generated Morse output

```bash
cat /data/home/qnxuser/morse_output/output.txt
```

---

## 6. Run the AI pipeline

```bash
python3 \
/data/home/qnxuser/projects/LumaLink_AI_Alert/lumalink_ai_server.py
```

Or launch both automatically:

```bash
./camera_example1_callback -u 1 && \
python3 /data/home/qnxuser/projects/LumaLink_AI_Alert/lumalink_ai_server.py
```

---

# Running the Transmitter

On a separate computer:

```bash
python3 transmitter.py
```

The transmitter converts a text message into Morse code and displays it as rapid flashes on the screen.

---

# Technologies Used

- QNX RTOS
- Raspberry Pi 5
- Raspberry Pi Camera Module 3 Wide
- C
- Python
- QNX Camera API (libcamapi)
- Qwen 3
- llama.cpp-qnx
- HTML/CSS/JavaScript

---

# Authors

- Omar
- Zayd
- Adam
