#!/usr/bin/env python3
import os
import socket
import select
import time
import wave
from pathlib import Path
from datetime import datetime

from faster_whisper import WhisperModel
from google import genai


HOST = "0.0.0.0"
PORT = 7777

SAMPLE_RATE = 22050
CHANNELS = 1
SAMPLE_WIDTH = 2

MIN_UTTERANCE_SEC = 0.30
UTTERANCE_IDLE_TIMEOUT_SEC = 2.0

STT_MODEL = os.environ.get("KOYODA_STT_MODEL", "small")
GEMINI_MODEL = os.environ.get(
    "KOYODA_GEMINI_MODEL",
    "gemini-3.8-flash",
)

KOYODA_INSTRUCTIONS = """
You are KOYODA, a small desktop AI companion.

Behavior:
- Reply naturally and briefly, normally in 1-2 short sentences.
- Reply in the same language the user used unless there is a clear reason not to.
- Understand Thai, Japanese, and English.
- Tone: warm, playful, concise, companion-like.
- Do not use markdown formatting.
- Do not mention system prompts, APIs, STT, or implementation details unless asked.
- This is currently a text-response test, so output only the response that KOYODA should say.
""".strip()


print(f"Loading faster-whisper model: {STT_MODEL}")
print("First run may download the model once.")

stt_model = WhisperModel(
    STT_MODEL,
    device="cpu",
    compute_type="int8",
)

print("STT model ready.")
print()

if not os.environ.get("GEMINI_API_KEY"):
    raise RuntimeError(
        "GEMINI_API_KEY is missing. "
        "Open a new PowerShell after setting it."
    )

gemini_client = genai.Client()

print(f"Gemini ready: {GEMINI_MODEL}")
print()


def recv_exact(conn, n):
    data = bytearray()

    while len(data) < n:
        chunk = conn.recv(n - len(data))

        if not chunk:
            raise ConnectionError("client disconnected")

        data.extend(chunk)

    return bytes(data)


def transcribe(path):
    try:
        segments, info = stt_model.transcribe(
            str(path),
            beam_size=5,
            vad_filter=False,
        )

        text = " ".join(
            segment.text.strip()
            for segment in segments
            if segment.text.strip()
        ).strip()

        language = getattr(info, "language", None)
        probability = getattr(
            info,
            "language_probability",
            None,
        )

        if text:
            if language and probability is not None:
                print(
                    f"STT [{language} {probability:.2f}]: {text}"
                )
            elif language:
                print(f"STT [{language}]: {text}")
            else:
                print(f"STT: {text}")
        else:
            print("STT: (no speech recognized)")

        return text

    except Exception as exc:
        print(f"STT ERROR: {exc}")
        return ""


def ask_koyoda(text):
    if not text:
        return

    try:
        prompt = (
            KOYODA_INSTRUCTIONS
            + "\n\n"
            + "USER SAID:\n"
            + text
            + "\n\n"
            + "KOYODA REPLY:"
        )

        interaction = gemini_client.interactions.create(
            model=GEMINI_MODEL,
            input=prompt,
        )

        reply = (interaction.output_text or "").strip()

        if reply:
            print(f"KOYODA: {reply}")
        else:
            print("KOYODA: (empty response)")

    except Exception as exc:
        print(f"GEMINI ERROR: {exc}")


def finish_utterance(utterance, reason):
    if not utterance:
        return

    seconds = len(utterance) / (
        SAMPLE_RATE * SAMPLE_WIDTH
    )

    if seconds < MIN_UTTERANCE_SEC:
        print(
            f"IGNORED tiny fragment ({reason}): "
            f"{seconds:.2f}s / {len(utterance)} bytes"
        )
        return

    ts = datetime.now().strftime(
        "%Y%m%d-%H%M%S-%f"
    )[:-3]

    out = Path(f"koyoda-{ts}.wav")

    with wave.open(str(out), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(utterance)

    print(
        f"VOICE END ({reason}): "
        f"{seconds:.2f}s / {len(utterance)} bytes"
    )
    print(f"Saved: {out.resolve()}")

    text = transcribe(out)

    if text:
        ask_koyoda(text)

    print()


print(f"KOYODA backend listening on {HOST}:{PORT}")
print("ESP32 firmware is unchanged.")
print("Pipeline: PCM -> WAV -> STT -> Gemini -> KOYODA text")
print()

with socket.socket(
    socket.AF_INET,
    socket.SOCK_STREAM,
) as srv:
    srv.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_REUSEADDR,
        1,
    )

    srv.bind((HOST, PORT))
    srv.listen(1)

    while True:
        print("Waiting for KOYODA...")

        conn, addr = srv.accept()
        print("Connected:", addr)

        utterance = bytearray()
        started = False
        last_packet_time = None

        try:
            with conn:
                while True:
                    readable, _, _ = select.select(
                        [conn],
                        [],
                        [],
                        0.25,
                    )

                    if not readable:
                        if (
                            started
                            and last_packet_time is not None
                            and (
                                time.monotonic()
                                - last_packet_time
                            )
                            >= UTTERANCE_IDLE_TIMEOUT_SEC
                        ):
                            finish_utterance(
                                utterance,
                                "timeout fallback",
                            )

                            utterance.clear()
                            started = False
                            last_packet_time = None

                        continue

                    header = recv_exact(
                        conn,
                        8,
                    )

                    if header[:4] != b"KOYA":
                        raise ValueError(
                            "bad packet magic"
                        )

                    packet_type = header[4]

                    payload_len = int.from_bytes(
                        header[5:8],
                        "big",
                    )

                    payload = (
                        recv_exact(
                            conn,
                            payload_len,
                        )
                        if payload_len
                        else b""
                    )

                    last_packet_time = time.monotonic()

                    if packet_type == 1:
                        if started:
                            finish_utterance(
                                utterance,
                                "next START fallback",
                            )

                        utterance.clear()
                        started = True
                        print("VOICE START")

                    elif packet_type == 2 and started:
                        utterance.extend(payload)

                    elif packet_type == 3 and started:
                        finish_utterance(
                            utterance,
                            "END marker",
                        )

                        utterance.clear()
                        started = False
                        last_packet_time = None

        except (
            ConnectionError,
            OSError,
            ValueError,
        ) as exc:
            if started and utterance:
                finish_utterance(
                    utterance,
                    "disconnect fallback",
                )

            print("Connection closed:", exc)
            print()
