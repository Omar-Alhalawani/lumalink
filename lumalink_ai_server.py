cd ~/projects/LumaLink_AI_Alert

cat > lumalink_ai_server.py <<'PY'
#!/usr/bin/env python3

import html
import json
import os
import re
import socket
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

INPUT_FILE = Path("/data/home/qnxuser/morse_output/output.txt")
DECODED_FILE = Path("/data/home/qnxuser/morse_output/decoded_message.txt")
SUMMARY_FILE = Path("/data/home/qnxuser/morse_output/ai_summary.txt")

LLAMA_CLI = Path("/data/home/qnxuser/llama.cpp/bin/llama-cli")
MODEL = Path(
    "/data/home/qnxuser/llama.cpp/models/"
    "qwen3-751.63M-Q4_K_M.gguf"
)

PORT = 8080

MORSE = {
    ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E",
    "..-.": "F", "--.": "G", "....": "H", "..": "I", ".---": "J",
    "-.-": "K", ".-..": "L", "--": "M", "-.": "N", "---": "O",
    ".--.": "P", "--.-": "Q", ".-.": "R", "...": "S", "-": "T",
    "..-": "U", "...-": "V", ".--": "W", "-..-": "X",
    "-.--": "Y", "--..": "Z",

    "-----": "0", ".----": "1", "..---": "2", "...--": "3",
    "....-": "4", ".....": "5", "-....": "6", "--...": "7",
    "---..": "8", "----.": "9",

    ".-.-.-": ".", "--..--": ",", "..--..": "?", "-.-.--": "!"
}


def decode_morse(raw):
    raw = raw.replace("–", "-").replace("—", "-").replace("·", ".")
    raw = re.sub(r"\s*/\s*", " / ", raw.strip())

    decoded_words = []

    for word in raw.split("/"):
        decoded_letters = []

        for symbol in word.strip().split():
            decoded_letters.append(MORSE.get(symbol, "?"))

        if decoded_letters:
            decoded_words.append("".join(decoded_letters))

    return " ".join(decoded_words)


def clean_ai_output(output):
    # Remove terminal colour codes.
    output = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", output)

    # Remove Qwen thinking if it appears.
    output = re.sub(
        r"<think>.*?</think>",
        "",
        output,
        flags=re.IGNORECASE | re.DOTALL
    )

    matches = re.findall(
        r"SUMMARY\s*:\s*([^\r\n]+)",
        output,
        flags=re.IGNORECASE
    )

    if matches:
        return matches[-1].strip().strip('`"')

    return ""


def run_qwen(decoded_message):
    prompt = f"""/no_think
You are the local AI assistant for LumaLink.

Read the decoded optical emergency message below.
Return exactly one short sentence for a rescue operator.

Do not explain.
Do not show reasoning.
Do not repeat these instructions.

Return exactly:
SUMMARY: your sentence

MESSAGE:
{decoded_message}
"""

    command = [
        str(LLAMA_CLI),
        "-m", str(MODEL),
        "--single-turn",
        "--no-display-prompt",
        "--log-disable",
        "--ctx-size", "512",
        "--no-warmup",
        "-n", "64",
        "--temp", "0",
        "-p", prompt
    ]

    environment = os.environ.copy()
    binary_directory = str(LLAMA_CLI.parent)

    current_library_path = environment.get("LD_LIBRARY_PATH", "")

    if current_library_path:
        environment["LD_LIBRARY_PATH"] = (
            binary_directory + ":" + current_library_path
        )
    else:
        environment["LD_LIBRARY_PATH"] = binary_directory

    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=environment,
        timeout=180
    )

    if result.returncode != 0:
        raise RuntimeError(
            f"llama-cli exited with code {result.returncode}"
        )

    summary = clean_ai_output(result.stdout)

    if not summary:
        raise RuntimeError("Qwen returned no usable SUMMARY line")

    return summary


def get_pi_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "10.113.69.106"
    finally:
        sock.close()


def build_page(raw_morse, decoded_message, ai_summary):
    raw_html = html.escape(raw_morse)
    decoded_html = html.escape(decoded_message)
    summary_html = html.escape(ai_summary)

    spoken_message = json.dumps(
        f"Emergency. SOS received. "
        f"{decoded_message}. {ai_summary}"
    )

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>LumaLink SOS</title>

<style>
body {{
    margin: 0;
    min-height: 100vh;
    display: grid;
    place-items: center;
    background: #170000;
    color: white;
    font-family: Arial, sans-serif;
    animation: flash 1s infinite;
}}

@keyframes flash {{
    0%, 100% {{ background: #170000; }}
    50% {{ background: #790000; }}
}}

main {{
    width: min(850px, 92vw);
    padding: 42px;
    text-align: center;
    border: 2px solid #ff5252;
    border-radius: 24px;
    background: rgba(8, 10, 16, 0.95);
    box-shadow: 0 0 60px rgba(255, 0, 0, 0.35);
}}

h1 {{
    color: #ff5a5a;
    font-size: clamp(2.5rem, 7vw, 5rem);
}}

.message {{
    padding: 24px;
    border-radius: 16px;
    background: rgba(255, 255, 255, 0.08);
    font-size: clamp(1.8rem, 5vw, 3.5rem);
    font-weight: bold;
}}

.summary {{
    margin: 26px 0;
    font-size: 1.35rem;
    line-height: 1.5;
}}

.raw {{
    color: #b9d7e8;
    font-family: monospace;
    overflow-wrap: anywhere;
}}

button {{
    margin: 8px;
    padding: 14px 20px;
    border: 0;
    border-radius: 12px;
    font-size: 1rem;
    font-weight: bold;
    cursor: pointer;
}}

.start {{
    background: #ff4141;
    color: white;
}}

.stop {{
    background: #225573;
    color: white;
}}
</style>
</head>

<body>
<main>
    <small>LUMALINK · QNX OPTICAL RECEIVER</small>

    <h1>🚨 SOS DETECTED</h1>

    <div class="message">{decoded_html}</div>

    <div class="summary">{summary_html}</div>

    <p class="raw">Raw Morse: {raw_html}</p>

    <button class="start" onclick="startAlarm()">
        Start alarm & read message
    </button>

    <button class="stop" onclick="stopAlarm()">
        Acknowledge
    </button>
</main>

<script>
let audioContext = null;
let alarmTimer = null;

const spokenMessage = {spoken_message};

function beep() {{
    if (!audioContext) return;

    const oscillator = audioContext.createOscillator();
    const gain = audioContext.createGain();

    oscillator.type = "square";
    oscillator.frequency.value = 880;

    gain.gain.setValueAtTime(
        0.18,
        audioContext.currentTime
    );

    gain.gain.exponentialRampToValueAtTime(
        0.0001,
        audioContext.currentTime + 0.3
    );

    oscillator.connect(gain);
    gain.connect(audioContext.destination);

    oscillator.start();
    oscillator.stop(audioContext.currentTime + 0.31);
}}

function startAlarm() {{
    if (!audioContext) {{
        audioContext = new (
            window.AudioContext ||
            window.webkitAudioContext
        )();
    }}

    audioContext.resume();

    beep();

    if (!alarmTimer) {{
        alarmTimer = setInterval(beep, 800);
    }}

    if ("speechSynthesis" in window) {{
        window.speechSynthesis.cancel();

        const speech = new SpeechSynthesisUtterance(
            spokenMessage
        );

        speech.rate = 0.9;

        window.speechSynthesis.speak(speech);
    }}
}}

function stopAlarm() {{
    if (alarmTimer) {{
        clearInterval(alarmTimer);
        alarmTimer = null;
    }}

    if ("speechSynthesis" in window) {{
        window.speechSynthesis.cancel();
    }}
}}

// Browsers may block this until the user clicks the button.
startAlarm();
</script>
</body>
</html>
""".encode("utf-8")


def start_server(raw_morse, decoded_message, ai_summary):
    page = build_page(raw_morse, decoded_message, ai_summary)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path not in ("/", "/index.html"):
                self.send_response(404)
                self.end_headers()
                return

            self.send_response(200)
            self.send_header(
                "Content-Type",
                "text/html; charset=utf-8"
            )
            self.send_header(
                "Content-Length",
                str(len(page))
            )
            self.send_header("Cache-Control", "no-store")
            self.end_headers()

            self.wfile.write(page)

        def log_message(self, format_string, *args):
            pass

    pi_ip = get_pi_ip()

    print()
    print(f"SOS web server: http://{pi_ip}:{PORT}/")
    print("Press Ctrl+C to stop.")

    server = HTTPServer(("0.0.0.0", PORT), Handler)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")
    finally:
        server.server_close()


def main():
    if not INPUT_FILE.is_file():
        print(f"ERROR: Missing file: {INPUT_FILE}", file=sys.stderr)
        return 1

    raw_morse = INPUT_FILE.read_text(
        encoding="utf-8",
        errors="replace"
    ).strip()

    if not raw_morse:
        print(f"ERROR: File is empty: {INPUT_FILE}", file=sys.stderr)
        return 1

    decoded_message = decode_morse(raw_morse)

    DECODED_FILE.write_text(
        decoded_message + "\n",
        encoding="utf-8"
    )

    print(f"Morse:   {raw_morse}")
    print(f"Message: {decoded_message}")

    print("Running local Qwen AI...")

    try:
        ai_summary = run_qwen(decoded_message)
        print(f"AI:      {ai_summary}")

    except Exception as error:
        print(f"AI warning: {error}", file=sys.stderr)

        if re.search(r"\bSOS\b", decoded_message, re.IGNORECASE):
            ai_summary = (
                "Critical distress signal received. "
                "Immediate assistance is required."
            )
        else:
            ai_summary = f"Optical message received: {decoded_message}"

        print(f"Fallback: {ai_summary}")

    SUMMARY_FILE.write_text(
        ai_summary + "\n",
        encoding="utf-8"
    )

    # The safety decision never depends on AI.
    if re.search(r"\bSOS\b", decoded_message, re.IGNORECASE):
        start_server(
            raw_morse,
            decoded_message,
            ai_summary
        )
    else:
        print("No SOS found. Web alarm server was not started.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
PY