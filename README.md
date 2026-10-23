# LumaLink — QNX Optical Emergency Communication

**QNX RTOS • Raspberry Pi 5 • C Camera API • Morse code • Python • Local AI**

LumaLink is a prototype **light-based message receiver** for scenarios where a camera can observe a remote flashing light source. A Python desktop transmitter encodes text as visible Morse flashes; a QNX-based camera application interprets light transitions, then a local Python pipeline decodes the message and generates a concise operator-facing summary. An **SOS** message can launch a browser-based alert display.

The main engineering challenge is **timing-sensitive optical decoding**: distinguishing dots, dashes, and gaps reliably despite camera frame-rate quantization, background light, and imprecise signal timing.

## End-to-end architecture

```text
Transmitting computer                         Receiving Raspberry Pi 5 / QNX
┌────────────────────────┐                    ┌──────────────────────────────┐
│ Python + Tkinter       │   light pulses     │ Camera Module 3 Wide         │
│ text → Morse → flashes ├───────────────────►│ QNX Camera API C receiver    │
└────────────────────────┘                    │ spot tracking + pulse timing  │
                                              └──────────────┬───────────────┘
                                                             │ output.txt
                                              ┌──────────────▼───────────────┐
                                              │ Python Morse decoder         │
                                              │ locally hosted Qwen/llama.cpp│
                                              │ short emergency summary      │
                                              │ SOS? → HTTP alert dashboard  │
                                              └──────────────────────────────┘
```

## Engineering highlights

- **Camera-based reception:** C application using QNX Camera API (`libcamapi`).
- **Localized optical signal tracking:** detect a bright spot through changes in local contrast rather than absolute frame brightness.
- **Transition validation:** confirm state changes across frames, associate timestamps with transitions, and tolerate isolated invalid pulses.
- **Timing-based Morse recognition:** transmitter uses nominal **100 ms dots**, **200 ms dashes**, and different symbol/letter/word gaps; receiver accounts for approximately 30 fps frame quantization.
- **Offline summarization:** Python invokes a locally hosted Qwen GGUF model via `llama-cli`; failures fall back to a deterministic text response.
- **Safety logic separated from AI:** the alert server is launched by detection of the decoded literal word **SOS**, not by the model's judgment.
- **Operator UI:** a generated web page presents decoded text, a short summary, and user-triggered audio alarm and speech synthesis.

This is an engineering demonstration, **not a validated emergency-response service**.

## Repository files

| File | Role |
| --- | --- |
| [`camera_example1_callback.c`](camera_example1_callback.c) | QNX camera receiver, spot tracking, pulse processing, Morse output |
| [`transmitter.py`](transmitter.py) | Tkinter-based text-to-light Morse transmitter |
| [`lumalink_ai_server.py`](lumalink_ai_server.py) | Decoding, local-model inference, and SOS web alert |
| `README.md` | Architecture, reproduction, and limitations |

## Hardware and software

**Receiver:** Raspberry Pi 5, Raspberry Pi Camera Module 3 Wide, QNX RTOS, QNX Camera API and QNX cross-compiler.

**Transmitter:** a separate computer with Python 3, Tkinter, and a screen aimed toward the camera.

**AI pipeline:** Python 3, QNX-compatible `llama.cpp` binary, and a compatible locally stored Qwen GGUF model.

## Running the transmitter

```bash
python3 transmitter.py
```

Enter a message in the dialog. The application opens a black window and flashes a small white dot. Press **Esc** to close. The dot/dash timing is customized for this demonstration and should not be treated as an interoperable general-purpose Morse transport specification.

## Building and running the receiver

In a configured QNX development environment, compile the C source for the target architecture:

```bash
qcc -Vgcc_ntoaarch64le camera_example1_callback.c -o camera_example1_callback -lcamapi
```

Copy the binary to the QNX device, make it executable, and run with the correct camera unit (shown here as unit 1):

```bash
chmod +x camera_example1_callback
./camera_example1_callback -u 1
```

The receiver expects a camera supported by the deployed QNX camera stack and writes Morse output to `/data/home/qnxuser/morse_output/output.txt`. Paths, device identifiers, and driver availability are **environment-specific**.

## Running the local AI/alert stage

The Python implementation references absolute QNX paths for the Morse output, `llama-cli`, and the model file. Configure those paths and install the model before running.

**Important source-file note:** the committed `lumalink_ai_server.py` currently contains surrounding shell heredoc lines (`cd ...`, `cat ... <<'PY'`, and closing `PY`). **As checked in, it is not directly runnable as a Python script**. Extract the enclosed Python into a clean script before execution, or remove those wrapper lines in a local copy. This README update intentionally does not modify the source.

The decoded message and summary are written to files. When decoded text contains **SOS**, an HTTP dashboard is served on port `8080` (subject to local networking and browser audio permissions).

## Limitations and opportunities

- No published automated reliability or packet-error measurements; optical performance depends on illumination, alignment, camera settings, frame rate, and timing.
- Camera selection, QNX cross-compilation, and local-model paths require device-specific configuration.
- The published AI file requires removal of shell wrapper text before direct Python execution.
- The repository is a proof of concept, not an authenticated, fault-tolerant, or field-certified distress communications product.

## Contributors

Omar, Zayd, and Adam.
