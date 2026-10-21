import os
import sys
import io
import time
from datetime import datetime
from flask import Flask, request, jsonify
from dotenv import load_dotenv
from google import genai
from google.genai import types
from groq import Groq
import paho.mqtt.publish as publish
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

# Đảm bảo in Tiếng Việt không lỗi trên Terminal Windows
sys.stdout.reconfigure(encoding='utf-8')

app = Flask(__name__)

env_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
load_dotenv(dotenv_path=env_path)


# ============================================================
# CẤU HÌNH AI & APIS
# ============================================================

gemini_api_key = os.getenv("GEMINI_API_KEY")

if not gemini_api_key:
    raise RuntimeError(
        "❌ Không tìm thấy GEMINI_API_KEY trong file .env"
    )

gemini_client = genai.Client(api_key=gemini_api_key)

GEMINI_MODEL = "gemini-2.5-flash"


# ============================================================
# GROQ
# ============================================================

groq_api_key = os.getenv("GROQ_API_KEY")

groq_client = Groq(api_key=groq_api_key) if groq_api_key else None

GROQ_WHISPER_MODEL = "whisper-large-v3-turbo"
GROQ_LLM_MODEL = "llama-3.3-70b-versatile"


# ============================================================
# MQTT
# ============================================================

MQTT_BROKER_IP = os.getenv(
    "MQTT_BROKER_IP",
    "127.0.0.1"
)

MQTT_TOPIC_SPEECH = os.getenv(
    "MQTT_TOPIC",
    "esp32/gemini"
)

MQTT_TOPIC_SENSOR = "esp32/dht"


# ============================================================
# VỊ TRÍ
# ============================================================

USER_LOCATION = os.getenv(
    "USER_LOCATION",
    "Thành phố Hồ Chí Minh, Việt Nam"
)


# ============================================================
# LỊCH SỬ HỘI THOẠI
# ============================================================

chat_history = []

MAX_HISTORY_TURNS = 6


# ============================================================
# NHIỆT ĐỘ PHÒNG
# ============================================================

current_room_temp = None


# ============================================================
# MQTT LISTENER
# ============================================================

def on_mqtt_message(client, userdata, msg):

    global current_room_temp

    try:

        payload = msg.payload.decode(
            'utf-8'
        ).strip()

        current_room_temp = payload

        print(
            f"🌡️ Cập nhật nhiệt độ phòng từ ESP32: "
            f"{current_room_temp}°C"
        )

    except Exception as e:

        print(
            f"⚠️ Lỗi đọc dữ liệu sensor: {e}"
        )


def start_mqtt_listener():

    client = mqtt.Client(
        CallbackAPIVersion.VERSION2
    )

    client.on_message = on_mqtt_message

    try:

        client.connect(
            MQTT_BROKER_IP,
            1883,
            60
        )

        client.subscribe(
            MQTT_TOPIC_SENSOR
        )

        client.loop_start()

        print(
            f"📡 Đã lắng nghe topic nhiệt độ "
            f"[{MQTT_TOPIC_SENSOR}]"
        )

    except Exception as e:

        print(
            f"⚠️ Lỗi kết nối MQTT Listener: {e}"
        )


start_mqtt_listener()


# ============================================================
# ENDPOINT NHẬN VOICE TỪ ESP32
# ============================================================

@app.route(
    "/voice",
    methods=["POST"]
)
def handle_voice():

    global chat_history
    global current_room_temp

    try:

        # ====================================================
        # KIỂM TRA FILE AUDIO
        # ====================================================

        if "file" not in request.files:

            return jsonify({
                "success": False,
                "error": "No file uploaded"
            }), 400


        audio_file = request.files["file"]


        if audio_file.filename == "":

            return jsonify({
                "success": False,
                "error": "Empty filename"
            }), 400


        print(
            f"🎙️ Nhận file ghi âm: "
            f"{audio_file.filename}"
        )


        audio_bytes = audio_file.read()


        # ====================================================
        # BƯỚC 1
        # WAV -> TEXT
        # GROQ WHISPER
        # ====================================================

        user_text = ""

        try:

            if not groq_client:

                raise Exception(
                    "GROQ_API_KEY chưa được cấu hình "
                    "trong file .env"
                )


            print(
                "🔄 Đang chuyển giọng nói thành "
                "văn bản bằng Groq Whisper..."
            )


            audio_io = (
                "rec.wav",
                io.BytesIO(audio_bytes),
                "audio/wav"
            )


            transcription = (
                groq_client.audio.transcriptions.create(
                    file=audio_io,
                    model=GROQ_WHISPER_MODEL,
                    language="vi"
                )
            )


            user_text = transcription.text.strip()


            print(
                f"🗣️ Người dùng hỏi: "
                f"\"{user_text}\""
            )


        except Exception as stt_err:

            print(
                f"❌ Lỗi chuyển đổi giọng nói: "
                f"{stt_err}"
            )

            return jsonify({
                "success": False,
                "error":
                    f"Lỗi Speech-to-Text: "
                    f"{str(stt_err)}"
            }), 500


        # ====================================================
        # KIỂM TRA TEXT
        # ====================================================

        if not user_text:

            print(
                "⚠️ Không nhận diện được âm thanh "
                "hoặc file rỗng."
            )

            return jsonify({
                "success": False,
                "error":
                    "Không thể nghe rõ câu hỏi"
            }), 400


        # ====================================================
        # BƯỚC 2
        # THỜI GIAN + NHIỆT ĐỘ + VỊ TRÍ
        # ====================================================

        weekdays = [
            "Thứ Hai",
            "Thứ Ba",
            "Thứ Tư",
            "Thứ Năm",
            "Thứ Sáu",
            "Thứ Bảy",
            "Chủ Nhật"
        ]


        now = datetime.now()


        current_time_str = (
            f"{weekdays[now.weekday()]}, "
            f"ngày {now.strftime('%d/%m/%Y %H:%M:%S')}"
        )


        if current_room_temp:

            temp_str = (
                f"{current_room_temp}°C"
            )

        else:

            temp_str = (
                "Chưa nhận được dữ liệu nhiệt độ"
            )


        # ====================================================
        # SYSTEM INSTRUCTION
        # ====================================================

        system_instruction = (

            "Bạn là trợ lý AI thông minh, "
            "hỗ trợ trả lời bằng tiếng Việt.\n"

            f"- Thời gian hiện tại: "
            f"{current_time_str}.\n"

            f"- Vị trí người dùng: "
            f"{USER_LOCATION}.\n"

            f"- Nhiệt độ phòng hiện tại "
            f"(đo thực tế từ cảm biến ESP32): "
            f"{temp_str}.\n"

            "- Bạn có quyền sử dụng Google Search "
            "để tra cứu Internet theo thời gian thực.\n"

            "- Với câu hỏi về tin tức mới nhất, "
            "sự kiện đang diễn ra, thời tiết, "
            "giá cả, tỷ giá, thể thao, lịch thi đấu, "
            "người nổi tiếng, sản phẩm mới, "
            "công nghệ mới, thông tin vừa cập nhật "
            "hoặc bất kỳ thông tin nào có thể đã "
            "thay đổi theo thời gian, hãy sử dụng "
            "Google Search trước khi trả lời.\n"

            "- Không được giả vờ biết thông tin mới "
            "nếu chưa kiểm tra.\n"

            "- Nếu thông tin không tìm thấy hoặc "
            "nguồn không đủ đáng tin cậy, "
            "hãy nói rõ điều đó.\n"

            "- Với câu hỏi kiến thức thông thường "
            "không cần dữ liệu mới, có thể trả lời "
            "trực tiếp.\n"

            "- Nếu người dùng hỏi về nhiệt độ phòng, "
            "hãy sử dụng thông tin nhiệt độ phòng "
            "đo thực tế ở trên.\n"

            "- Trả lời ngắn gọn, chính xác, "
            "tự nhiên để đọc qua loa phát thanh."
        )


        # ====================================================
        # LỊCH SỬ HỘI THOẠI
        # ====================================================

        history_context = "\n".join(
            chat_history
        ) if chat_history else ""


        if history_context:

            prompt_with_history = (
                f"Lịch sử hội thoại:\n"
                f"{history_context}\n\n"
                f"Người dùng: {user_text}"
            )

        else:

            prompt_with_history = user_text


        # ====================================================
        # BƯỚC 3
        # GEMINI + GOOGLE SEARCH
        # ====================================================

        response_text = ""

        used_model = GEMINI_MODEL


        try:

            print(
                f"🤖 Đang gửi câu hỏi tới Gemini "
                f"({GEMINI_MODEL})..."
            )


            # =================================================
            # GOOGLE SEARCH GROUNDING
            # =================================================

            google_search_tool = types.Tool(
                google_search=types.GoogleSearch()
            )


            config = types.GenerateContentConfig(

                temperature=0.7,

                system_instruction=
                    system_instruction,

                tools=[
                    google_search_tool
                ]
            )


            # =================================================
            # GỌI GEMINI
            # =================================================

            response = (
                gemini_client.models.generate_content(

                    model=GEMINI_MODEL,

                    contents=prompt_with_history,

                    config=config
                )
            )


            if response.text:

                response_text = (
                    response.text.strip()
                )

            else:

                response_text = (
                    "Tôi không nghe rõ, "
                    "bạn nói lại được không?"
                )


        except Exception as gemini_err:

            # =================================================
            # FALLBACK GROQ
            # =================================================

            print(
                f"⚠️ Gemini/Google Search gặp lỗi: "
                f"{gemini_err}"
            )

            print(
                "⚠️ Đang chuyển sang Groq Fallback..."
            )

            print(
                "⚠️ Groq Fallback hiện không có "
                "Google Search Grounding."
            )


            used_model = GROQ_LLM_MODEL


            if not groq_client:

                raise Exception(
                    "Gemini lỗi và GROQ_API_KEY "
                    "chưa được cấu hình."
                )


            messages = [

                {
                    "role": "system",
                    "content":
                        system_instruction
                },

                {
                    "role": "user",
                    "content":
                        prompt_with_history
                }

            ]


            chat_completion = (
                groq_client.chat.completions.create(

                    messages=messages,

                    model=GROQ_LLM_MODEL,

                    temperature=0.7
                )
            )


            response_text = (
                chat_completion
                .choices[0]
                .message
                .content
                .strip()
            )


        # ====================================================
        # IN CÂU TRẢ LỜI
        # ====================================================

        print(
            f"💬 Câu trả lời AI "
            f"({used_model}): "
            f"{response_text}"
        )


        # ====================================================
        # BƯỚC 4
        # LƯU LỊCH SỬ
        # ====================================================

        chat_history.append(
            f"User: {user_text}"
        )

        chat_history.append(
            f"AI: {response_text}"
        )


        if len(chat_history) > MAX_HISTORY_TURNS:

            chat_history = (
                chat_history[-MAX_HISTORY_TURNS:]
            )


        # ====================================================
        # BƯỚC 5
        # GỬI KẾT QUẢ QUA MQTT
        # ====================================================

        try:

            publish.single(

                topic=MQTT_TOPIC_SPEECH,

                payload=
                    response_text.encode(
                        'utf-8'
                    ),

                hostname=MQTT_BROKER_IP,

                port=1883
            )


            print(
                f"📢 Đã gửi phản hồi qua MQTT "
                f"[{MQTT_TOPIC_SPEECH}]!\n"
            )


        except Exception as mqtt_err:

            print(
                f"⚠️ Lỗi gửi MQTT: "
                f"{mqtt_err}\n"
            )


        # ====================================================
        # TRẢ JSON VỀ ESP32 / CLIENT
        # ====================================================

        return jsonify({

            "success": True,

            "user_text": user_text,

            "answer": response_text,

            "model": used_model

        }), 200


    except Exception as e:

        print(
            f"❌ Lỗi xử lý Server: "
            f"{str(e)}"
        )


        return jsonify({

            "success": False,

            "error": str(e)

        }), 500


# ============================================================
# MAIN
# ============================================================

if __name__ == "__main__":

    print(
        "🚀 Đang khởi chạy Voice Server..."
    )

    app.run(
        host="0.0.0.0",
        port=5000,
        debug=True
    )