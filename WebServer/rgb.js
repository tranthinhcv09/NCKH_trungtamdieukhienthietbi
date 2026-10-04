/**
 * SMART HOME DASHBOARD - CORE CONTROLLER & MQTT ENGINE
 * Handles user-configured devices, MQTT, voice control, and dashboard UI.
 */

// ==========================================
// 1. USER DATA & STATE MANAGEMENT
// ==========================================

function readStoredArray(key) {
  try {
    const stored = localStorage.getItem(key);
    const parsed = stored ? JSON.parse(stored) : null;
    return Array.isArray(parsed) ? parsed : [];
  } catch (error) {
    console.warn(`Không thể đọc dữ liệu đã lưu cho ${key}:`, error);
    return [];
  }
}

const DEVICE_DATABASE_NAME = "smart-home-dashboard";
const DEVICE_DATABASE_VERSION = 1;
const DEVICE_STORE_NAME = "app-data";
const DEVICE_RECORD_KEY = "devices";

const LEGACY_DEFAULT_ROOM_IDS = new Set([
  "living-room",
  "bedroom",
  "kitchen",
  "garden",
]);
const LEGACY_DEFAULT_DEVICE_IDS = new Set([
  "device_rgb_main",
  "device_relay_light",
  "device_living_fan",
  "device_dht_sensor",
  "device_curtain_main",
]);

function removeLegacyDefaults(rooms, devices) {
  return {
    rooms: rooms.filter((room) => !LEGACY_DEFAULT_ROOM_IDS.has(room.id)),
    devices: devices.filter(
      (device) =>
        !device.isBuiltin && !LEGACY_DEFAULT_DEVICE_IDS.has(device.id),
    ),
  };
}

function normalizeDevices(devices) {
  devices.forEach((device) => {
    if (device.type === "rgb") {
      if (!device.speechTopic) device.speechTopic = "esp32/speech";
      if (!device.pubTopic) device.pubTopic = "esp32/rgb";
      if (!device.savedColor) device.savedColor = device.color || "#3b82f6";
    }
    if (device.type === "curtain" && device.position === undefined) {
      device.position = 0;
    }
    if (device.type === "fan") {
      const oldSpeed = Number(device.speed);
      device.speed = [1, 2, 3].includes(oldSpeed)
        ? oldSpeed * 25
        : [25, 50, 75, 100].includes(oldSpeed)
          ? oldSpeed
          : 25;
    }
  });
  return devices;
}

let deviceDatabasePromise;

function openDeviceDatabase() {
  if (deviceDatabasePromise) return deviceDatabasePromise;

  deviceDatabasePromise = new Promise((resolve, reject) => {
    if (!window.indexedDB) {
      reject(new Error("Trình duyệt không hỗ trợ IndexedDB."));
      return;
    }

    const request = window.indexedDB.open(
      DEVICE_DATABASE_NAME,
      DEVICE_DATABASE_VERSION,
    );
    request.onupgradeneeded = () => {
      request.result.createObjectStore(DEVICE_STORE_NAME, { keyPath: "key" });
    };
    request.onsuccess = () => {
      const database = request.result;
      database.onversionchange = () => {
        database.close();
        deviceDatabasePromise = null;
      };
      resolve(database);
    };
    request.onerror = () => {
      deviceDatabasePromise = null;
      reject(request.error || new Error("Không thể mở IndexedDB."));
    };
    request.onblocked = () => {
      deviceDatabasePromise = null;
      reject(new Error("IndexedDB đang bị chặn bởi một kết nối khác."));
    };
  });

  return deviceDatabasePromise;
}

async function readDevicesFromDatabase() {
  const database = await openDeviceDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(DEVICE_STORE_NAME, "readonly");
    const request = transaction
      .objectStore(DEVICE_STORE_NAME)
      .get(DEVICE_RECORD_KEY);
    request.onsuccess = () => resolve(request.result?.devices ?? null);
    request.onerror = () =>
      reject(request.error || new Error("Không thể đọc danh sách thiết bị."));
    transaction.onabort = () =>
      reject(
        transaction.error || new Error("Không thể đọc danh sách thiết bị."),
      );
  });
}

async function writeDevicesToDatabase(devices) {
  const database = await openDeviceDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(DEVICE_STORE_NAME, "readwrite");
    transaction.objectStore(DEVICE_STORE_NAME).put({
      key: DEVICE_RECORD_KEY,
      devices,
    });
    transaction.oncomplete = resolve;
    transaction.onerror = () =>
      reject(
        transaction.error || new Error("Không thể lưu danh sách thiết bị."),
      );
    transaction.onabort = () =>
      reject(
        transaction.error || new Error("Không thể lưu danh sách thiết bị."),
      );
  });
}

async function loadDevices() {
  let devices = await readDevicesFromDatabase();
  if (devices === null) {
    const legacyValue = localStorage.getItem("smarthome_devices");
    let canRemoveLegacyValue = !legacyValue;
    if (legacyValue) {
      try {
        const legacyDevices = JSON.parse(legacyValue);
        if (Array.isArray(legacyDevices)) {
          devices = removeLegacyDefaults([], legacyDevices).devices;
          canRemoveLegacyValue = true;
        } else {
          console.warn("Dữ liệu thiết bị cũ không phải một danh sách.");
          devices = [];
        }
      } catch (error) {
        console.warn("Không thể đọc dữ liệu thiết bị cũ:", error);
        devices = [];
      }
    } else {
      devices = [];
    }

    await writeDevicesToDatabase(normalizeDevices(devices));
    if (canRemoveLegacyValue) {
      localStorage.removeItem("smarthome_devices");
    }
  }

  if (!Array.isArray(devices)) {
    throw new Error("Dữ liệu thiết bị trong IndexedDB không hợp lệ.");
  }
  return normalizeDevices(devices);
}

let state = {
  rooms: readStoredArray("smarthome_rooms"),
  devices: [],
  activeRoom: "all",
  searchQuery: "",
  theme: localStorage.getItem("smarthome_theme") || "dark",
  brokerUrl: localStorage.getItem("mqttBrokerUrl") || "ws://192.168.1.6:9001",
  isListeningVoice: false,
  editingDeviceId: null,
};

state.rooms = removeLegacyDefaults(state.rooms, []).rooms;

function saveState() {
  try {
    localStorage.setItem("smarthome_rooms", JSON.stringify(state.rooms));
  } catch (error) {
    console.error("Không thể lưu dữ liệu phòng:", error);
    showToast("Không thể lưu danh sách phòng trên trình duyệt này!", "error");
  }

  return writeDevicesToDatabase(state.devices).catch((error) => {
    console.error("Không thể lưu thiết bị vào IndexedDB:", error);
    showToast("Không thể lưu thiết bị vào cơ sở dữ liệu!", "error");
  });
}

// ==========================================
// 2. MQTT CLIENT ENGINE
// ==========================================

let client;

function setQuickBroker(url) {
  const input = document.getElementById("brokerUrlInput");
  if (input) input.value = url;
  connectBroker(url);
  closeModal("mqttModal");
}

function connectBroker(targetUrl) {
  const brokerInput = document.getElementById("brokerUrlInput");
  const brokerUrl = (
    targetUrl ||
    (brokerInput ? brokerInput.value : "") ||
    localStorage.getItem("mqttBrokerUrl") ||
    "ws://10.251.10.110:9001"
  ).trim();

  if (!/^wss?:\/\/[^\s]+$/i.test(brokerUrl)) {
    updateStatusUI("error", "URL sai định dạng");
    showToast("Địa chỉ Broker phải bắt đầu bằng ws:// hoặc wss://", "error");
    return;
  }

  localStorage.setItem("mqttBrokerUrl", brokerUrl);
  state.brokerUrl = brokerUrl;
  if (brokerInput) brokerInput.value = brokerUrl;

  disconnectBroker(false);

  updateStatusUI("connecting", "Đang kết nối...");
  addLog("SYS", "Đang kết nối tới: " + brokerUrl, "in");

  try {
    client = mqtt.connect(brokerUrl);

    client.on("connect", () => {
      updateStatusUI("connected", "Đã kết nối");
      showToast("Kết nối MQTT Broker thành công!", "success");
      addLog("SYS", "Đã kết nối thành công tới: " + brokerUrl, "out");

      // Đăng ký toàn bộ topics
      subscribeAllTopics();
    });

    client.on("message", (topic, message) => {
      handleIncomingMQTT(topic, message.toString());
    });

    client.on("error", (err) => {
      updateStatusUI("error", "Không thể kết nối");
      console.error("Lỗi kết nối MQTT:", err);
      addLog("ERR", "Lỗi kết nối: " + (err.message || err), "in");
    });

    client.on("close", () => {
      if (client && !client.connected) {
        updateStatusUI("connecting", "Mất kết nối (Đang thử lại...)");
      }
    });
  } catch (err) {
    updateStatusUI("error", "Không thể kết nối");
    showToast("Lỗi kết nối MQTT: " + err.message, "error");
  }
}

function disconnectBroker(showNotification = true) {
  if (client) {
    try {
      client.end(true);
      client = null;
      updateStatusUI("error", "Đã ngắt kết nối");
      if (showNotification) {
        showToast("Đã ngắt kết nối MQTT Broker!", "info");
        addLog("SYS", "Đã chủ động ngắt kết nối MQTT", "out");
      }
    } catch (e) {
      console.error(e);
    }
  }
}

function initMQTT() {
  connectBroker();
}

function subscribeAllTopics() {
  if (!client || !client.connected) return;

  const topicsToSub = new Set([
    "esp32/send",
    "esp32/web",
    "esp32/dht",
    "esp32/fan",
    "esp32/curtain",
  ]);
  state.devices.forEach((d) => {
    if (d.subTopic) topicsToSub.add(d.subTopic.trim());
  });

  topicsToSub.forEach((topic) => {
    if (topic) {
      client.subscribe(topic, (err) => {
        if (!err) addLog("SUB", "Đã lắng nghe: " + topic, "out");
      });
    }
  });
}

function publishMQTT(topic, payload) {
  if (!topic) return;
  const msgStr =
    typeof payload === "object" ? JSON.stringify(payload) : String(payload);

  if (typeof client !== "undefined" && client && client.connected) {
    client.publish(topic, msgStr);
    addLog(topic, msgStr, "out");
    console.log("📤 MQTT Gửi:", topic, msgStr);
  } else {
    addLog(topic, "[CHƯA KẾT NỐI] " + msgStr, "out");
    showToast("Chưa kết nối MQTT Broker!", "error");
  }
}

function updateStatusUI(statusClass, text) {
  const badge = document.getElementById("brokerBadge");
  const txt = document.getElementById("brokerStatusText");
  const modalTxt = document.getElementById("modalBrokerStatus");
  if (badge) {
    badge.className = `status-badge ${statusClass}`;
  }
  if (txt) txt.textContent = text;
  if (modalTxt) {
    modalTxt.textContent = text;
    modalTxt.className =
      statusClass === "connected"
        ? "da-ket-noi"
        : statusClass === "connecting"
          ? "dang-ket-noi"
          : "loi";
  }
}

// ==========================================
// 3. INCOMING MQTT MESSAGE HANDLER
// ==========================================

function handleIncomingMQTT(topic, rawMsg) {
  const msg = rawMsg.trim();
  addLog(topic, msg, "in");

  const speechDevice = state.devices.find(
    (dev) =>
      dev.type === "rgb" && topic === (dev.speechTopic || "esp32/speech"),
  );
  if (speechDevice) {
    try {
      const color = JSON.parse(msg);
      const channels = [color.r, color.g, color.b].map(Number);
      if (
        channels.every(
          (channel) =>
            Number.isFinite(channel) && channel >= 0 && channel <= 255,
        )
      ) {
        const colorName = getColorName(
          rgbToHex(channels[0], channels[1], channels[2]),
        );
        publishMQTT(speechDevice.speechTopic || "esp32/speech", {
          text: colorName,
        });
      }
    } catch (error) {
      console.warn("Không thể phân tích màu RGB từ MQTT:", error);
    }
  }

  let shouldRender = false;
  state.devices.forEach((dev) => {
    // Check RGB devices
    if (
      dev.type === "rgb" &&
      (topic === dev.subTopic || topic === "esp32/send")
    ) {
      shouldRender = true;
      const upMsg = msg.toUpperCase();
      if (
        upMsg === "TO_ON" ||
        upMsg === "ON" ||
        upMsg === "1" ||
        upMsg === "ONRGB"
      ) {
        dev.isOn = true;
        if (dev.color === "#000000") dev.color = "#ffffff";
      } else if (
        upMsg === "TO_OFF" ||
        upMsg === "OFF" ||
        upMsg === "0" ||
        upMsg === "OFFRGB"
      ) {
        dev.isOn = false;
      }
    }

    // Check Switch devices
    if (
      dev.type === "switch" &&
      (topic === dev.subTopic || topic === "esp32/web")
    ) {
      shouldRender = true;
      const upMsg = msg.toUpperCase();
      if (upMsg === "ON" || upMsg === "1" || upMsg === "TO_ON") {
        dev.isOn = true;
      } else if (upMsg === "OFF" || upMsg === "0" || upMsg === "TO_OFF") {
        dev.isOn = false;
      }
    }

    // Check Fan devices
    if (dev.type === "fan" && topic === dev.subTopic) {
      shouldRender = true;
      const normalizedMsg = msg.toLowerCase();
      const fanOnMatch = normalizedMsg.match(/^fan_on_(25|50|75|100)$/);
      const oldSpeedMatch = normalizedMsg.match(/^fan_speed_(\d+)$/);
      let speed = fanOnMatch ? Number(fanOnMatch[1]) : null;

      if (oldSpeedMatch) {
        const oldSpeed = Number(oldSpeedMatch[1]);
        speed = oldSpeed > 0 && oldSpeed <= 3 ? oldSpeed * 25 : oldSpeed;
      } else if (/^\d+$/.test(normalizedMsg)) {
        const oldSpeed = Number(normalizedMsg);
        speed = oldSpeed > 0 && oldSpeed <= 3 ? oldSpeed * 25 : oldSpeed;
      }

      if (normalizedMsg === "fan_off" || speed === 0) {
        dev.isOn = false;
      } else if (speed !== null && speed > 0) {
        dev.isOn = true;
        dev.speed = speed;
      } else {
        dev.isOn = normalizedMsg === "on";
      }
    }

    // Check Curtain devices
    if (dev.type === "curtain" && topic === dev.subTopic) {
      shouldRender = true;
      if (!isNaN(msg)) {
        dev.position = Math.min(100, Math.max(0, parseInt(msg)));
      } else {
        const up = msg.toUpperCase();
        if (up === "OPEN") dev.position = 100;
        if (up === "CLOSE") dev.position = 0;
      }
    }

    // Check Sensor devices
    if (dev.type === "sensor" && topic === dev.subTopic) {
      shouldRender = true;
      try {
        const data = JSON.parse(msg);
        if (data.temp !== undefined) dev.temp = parseFloat(data.temp);
        if (data.hum !== undefined) dev.hum = parseFloat(data.hum);
      } catch (e) {
        const tempMatch = msg.match(/temp[:=]\s*([\d.]+)/i);
        const humMatch = msg.match(/hum[:=]\s*([\d.]+)/i);
        if (tempMatch) dev.temp = parseFloat(tempMatch[1]);
        if (humMatch) dev.hum = parseFloat(humMatch[1]);
      }
    }
  });

  if (shouldRender) renderDevices();
}

// ==========================================
// 4. RGB SPECIFIC HELPER LOGIC
// ==========================================

function hexToRgb(hex) {
  let c = hex.replace("#", "");
  if (c.length === 3)
    c = c
      .split("")
      .map((x) => x + x)
      .join("");
  const num = parseInt(c, 16) || 0;
  return {
    r: (num >> 16) & 255,
    g: (num >> 8) & 255,
    b: num & 255,
  };
}

function rgbToHex(r, g, b) {
  const toHex = (n) => {
    const clamped = Math.max(0, Math.min(255, Math.round(n)));
    return clamped.toString(16).padStart(2, "0");
  };
  return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

let rgbDebounceTimers = {};
function debouncedSendRGB(dev) {
  if (rgbDebounceTimers[dev.id]) clearTimeout(rgbDebounceTimers[dev.id]);
  rgbDebounceTimers[dev.id] = setTimeout(() => {
    sendRGBDeviceUpdate(dev);
  }, 40);
}

function getColorName(hex) {
  const { r, g, b } = hexToRgb(hex);

  if (r < 20 && g < 20 && b < 20) return "Đen";
  if (r > 200 && g > 200 && b > 200) return "Trắng";
  if (r > 200 && g < 100 && b < 100) return "Đỏ";
  if (g > 200 && r < 100 && b < 100) return "Xanh lá";
  if (b > 200 && r < 100 && g < 100) return "Xanh dương";
  if (r > 200 && g > 200 && b < 100) return "Vàng";
  if (r > 200 && b > 200 && g < 100) return "Tím / Hồng";
  if (g > 200 && b > 200 && r < 100) return "Xanh ngọc";
  if (r > 200 && g > 100 && b < 50) return "Cam";
  return "Màu tùy chỉnh";
}

function sendRGBDeviceUpdate(dev, isSaving = false) {
  let hex = dev.isOn ? dev.color : "#000000";
  const { r: baseR, g: baseG, b: baseB } = hexToRgb(hex);
  const r = Math.round(baseR * (dev.brightness / 100));
  const g = Math.round(baseG * (dev.brightness / 100));
  const b = Math.round(baseB * (dev.brightness / 100));

  const colorName = dev.isOn ? getColorName(dev.color) : "Đã tắt";

  // Luôn gửi RGB sang esp32/rgb để đèn đổi màu ngay
  publishMQTT(dev.pubTopic || "esp32/rgb", { r, g, b });

  // Gửi text giọng nói khi isSaving = true
  if (isSaving) {
    const speechTopic = dev.speechTopic || "esp32/speech";
    publishMQTT(speechTopic, { text: colorName });
    addLog(speechTopic, `[GIỌNG NÓI] Đã gửi text: "${colorName}"`, "out");
  }

  saveState();
}

// ==========================================
// 5. VOICE CONTROL & SPEECH RECOGNITION (WEB SPEECH API)
// ==========================================

let recognition = null;

function initVoiceRecognition() {
  const SpeechRecognition =
    window.SpeechRecognition || window.webkitSpeechRecognition;
  if (!SpeechRecognition) {
    showToast("Trình duyệt không hỗ trợ Web Speech API", "error");
    return null;
  }

  const rec = new SpeechRecognition();
  rec.lang = "vi-VN";
  rec.continuous = false;
  rec.interimResults = false;

  rec.onstart = () => {
    state.isListeningVoice = true;
    updateVoiceUI(true);
    showToast("Đang lắng nghe giọng nói...", "info");
  };

  rec.onresult = (event) => {
    const transcript = event.results[0][0].transcript.toLowerCase().trim();
    addLog("VOICE", `Nhận diện: "${transcript}"`, "in");
    showToast(`Đã nghe: "${transcript}"`, "success");
    processVoiceCommand(transcript);
  };

  rec.onerror = (event) => {
    console.error("Lỗi giọng nói:", event.error);
    showToast("Lỗi nhận diện giọng nói: " + event.error, "error");
    state.isListeningVoice = false;
    updateVoiceUI(false);
  };

  rec.onend = () => {
    state.isListeningVoice = false;
    updateVoiceUI(false);
  };

  return rec;
}

function toggleVoiceControl() {
  if (!recognition) recognition = initVoiceRecognition();
  if (!recognition) return;

  if (state.isListeningVoice) {
    recognition.stop();
  } else {
    try {
      recognition.start();
    } catch (e) {
      console.error(e);
    }
  }
}

function updateVoiceUI(isListening) {
  const btn = document.getElementById("voiceControlBtn");
  if (btn) {
    btn.classList.toggle("listening", isListening);
    btn.innerHTML = isListening ? "🎙️ Đang nghe..." : "🎤 Giọng nói";
  }
}

function speakText(text) {
  if ("speechSynthesis" in window) {
    const utterance = new SpeechSynthesisUtterance(text);
    utterance.lang = "vi-VN";
    window.speechSynthesis.speak(utterance);
  }
}

function processVoiceCommand(cmd) {
  // 1. Phân tích ngữ cảnh Kịch bản (Scenes)
  if (cmd.includes("về nhà") || cmd.includes("home")) {
    activateScene("home");
    speakText("Đã mở chế độ về nhà");
    return;
  }
  if (cmd.includes("đi ngủ") || cmd.includes("chúc ngủ ngon")) {
    activateScene("sleep");
    speakText("Đã kích hoạt chế độ đi ngủ");
    return;
  }
  if (cmd.includes("thư giãn")) {
    activateScene("relax");
    speakText("Đã kích hoạt chế độ thư giãn");
    return;
  }
  if (cmd.includes("tắt hết") || cmd.includes("tắt tất cả")) {
    activateScene("all_off");
    speakText("Đã tắt toàn bộ thiết bị");
    return;
  }

  // 2. Bật / Tắt theo lệnh cụ thể
  let handled = false;
  let responseText = "";
  let colorCommandHandled = false;
  state.devices.forEach((dev) => {
    const nameLower = dev.name.toLowerCase();

    // Lệnh cho đèn RGB / Đèn màu
    if (
      dev.type === "rgb" &&
      !cmd.includes("tắt") &&
      (cmd.includes("màu") || cmd.includes("đèn"))
    ) {
      let selectedColorName = "";
      if (cmd.includes("đỏ")) {
        changeRGBColor(dev.id, "#ef4444");
        selectedColorName = "đỏ";
      } else if (cmd.includes("xanh lá")) {
        changeRGBColor(dev.id, "#22c55e");
        selectedColorName = "xanh lá";
      } else if (cmd.includes("xanh dương") || cmd.includes("xanh biển")) {
        changeRGBColor(dev.id, "#3b82f6");
        selectedColorName = "xanh dương";
      } else if (cmd.includes("vàng")) {
        changeRGBColor(dev.id, "#eab308");
        selectedColorName = "vàng";
      } else if (cmd.includes("tím")) {
        changeRGBColor(dev.id, "#a855f7");
        selectedColorName = "tím";
      } else if (cmd.includes("trắng")) {
        changeRGBColor(dev.id, "#ffffff");
        selectedColorName = "trắng";
      } else if (cmd.includes("cam")) {
        changeRGBColor(dev.id, "#f97316");
        selectedColorName = "cam";
      }
      if (selectedColorName) {
        handled = true;
        colorCommandHandled = true;
        responseText = `Mở đèn màu ${selectedColorName}`;
      }
    }

    // Lệnh Bật / Tắt chung
    if (
      !colorCommandHandled &&
      (cmd.includes("bật") || cmd.includes("mở")) &&
      (cmd.includes(nameLower) || cmd.includes("đèn") || cmd.includes("quạt"))
    ) {
      if (dev.type === "switch") toggleSwitchDevice(dev.id, true);
      if (dev.type === "rgb") toggleRGBPower(dev.id, true);
      if (dev.type === "fan") toggleFanDevice(dev.id, true);
      handled = true;
      if (dev.type === "switch" || dev.type === "rgb") {
        responseText = "Mở đèn";
      }
    } else if (
      !colorCommandHandled &&
      cmd.includes("tắt") &&
      (cmd.includes(nameLower) || cmd.includes("đèn") || cmd.includes("quạt"))
    ) {
      if (dev.type === "switch") toggleSwitchDevice(dev.id, false);
      if (dev.type === "rgb") toggleRGBPower(dev.id, false);
      if (dev.type === "fan") toggleFanDevice(dev.id, false);
      handled = true;
      if (dev.type === "switch" || dev.type === "rgb") {
        responseText = "Tắt đèn";
      }
    }

    // Lệnh Rèm cửa
    if (
      dev.type === "curtain" &&
      (cmd.includes("rèm") || cmd.includes("màn"))
    ) {
      if (cmd.includes("mở")) setCurtainPosition(dev.id, 100);
      else if (cmd.includes("đóng") || cmd.includes("khép"))
        setCurtainPosition(dev.id, 0);
      handled = true;
    }
  });

  if (handled) {
    speakText(responseText || "Đã thực hiện lệnh");
  } else {
    speakText("Không tìm thấy thiết bị phù hợp");
    showToast("Không tìm thấy lệnh hoặc thiết bị tương ứng!", "info");
  }
}

// ==========================================
// 6. DEVICE RENDERING & UI GENERATOR
// ==========================================

function renderRooms() {
  const container = document.getElementById("roomTabs");
  if (!container) return;

  const rooms = [
    { id: "all", name: "Tất cả" },
    ...state.rooms.filter((room) => room.id !== "all"),
  ];

  container.innerHTML = rooms
    .map(
      (room) => `
    <button class="room-tab ${state.activeRoom === room.id ? "active" : ""}" onclick="selectRoom('${room.id}')">
      ${room.name}
    </button>
  `,
    )
    .join("");
}

function selectRoom(roomId) {
  state.activeRoom = roomId;
  renderRooms();
  renderDevices();
}

function handleSearchDevices(query) {
  state.searchQuery = query.toLowerCase().trim();
  renderDevices();
}

function renderDevices() {
  const container = document.getElementById("devicesGrid");
  if (!container) return;

  let filtered =
    state.activeRoom === "all"
      ? state.devices
      : state.devices.filter((d) => d.room === state.activeRoom);

  if (state.searchQuery) {
    filtered = filtered.filter(
      (d) =>
        d.name.toLowerCase().includes(state.searchQuery) ||
        (d.pubTopic && d.pubTopic.toLowerCase().includes(state.searchQuery)),
    );
  }

  if (filtered.length === 0) {
    container.innerHTML = `
      <div class="empty-state">
        <svg fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M12 9v3m0 0v3m0-3h3m-3 0H9m12 0a9 9 0 11-18 0 9 9 0 0118 0z"></path></svg>
        <h3>Chưa có thiết bị nào phù hợp</h3>
        <p style="margin-top:6px; font-size:0.85rem;">Bấm nút "+ Thêm thiết bị" bên trên để tạo thiết bị mới.</p>
      </div>
    `;
    return;
  }

  container.innerHTML = filtered
    .map((dev) => generateDeviceCardHTML(dev))
    .join("");
}

function generateDeviceCardHTML(dev) {
  const roomObj = state.rooms.find((r) => r.id === dev.room);
  const roomName = roomObj ? roomObj.name : "Nhà";

  // Action Buttons cho từng card (Sửa & Xóa)
  const cardActionControls = `
    <button class="card-opt-btn" onclick="openEditDeviceModal('${dev.id}')" title="Chỉnh sửa thiết bị">✏️</button>
    <button class="card-opt-btn" onclick="deleteDevice('${dev.id}')" title="Xóa thiết bị">✕</button>
  `;

  // 1. RGB Device Card
  if (dev.type === "rgb") {
    const colorName = getColorName(dev.color);
    const { r, g, b } = hexToRgb(dev.color);
    const isUnsaved =
      dev.color.toLowerCase() !== (dev.savedColor || "").toLowerCase();
    const presets = [
      "#ffffff",
      "#ef4444",
      "#22c55e",
      "#3b82f6",
      "#eab308",
      "#f97316",
      "#a855f7",
      "#06b6d4",
    ];

    return `
      <div class="device-card ${dev.isOn ? "is-active" : ""}" id="card_${dev.id}">
        <div class="card-header">
          <div class="card-title-group">
            <div class="device-icon-box" style="${dev.isOn ? `background:${dev.color}22; color:${dev.color};` : ""}">
              <svg fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z"></path></svg>
            </div>
            <div>
              <div class="device-title">${dev.name}</div>
              <div class="device-subtitle">${roomName} • ${dev.pubTopic}</div>
            </div>
          </div>
          <div class="card-actions">
            <label class="switch">
              <input type="checkbox" id="switch_${dev.id}" ${dev.isOn ? "checked" : ""} onchange="toggleRGBPower('${dev.id}', this.checked)">
              <span class="slider"></span>
            </label>
            ${cardActionControls}
          </div>
        </div>

        <div class="rgb-preview-banner" id="banner_${dev.id}" style="background: ${dev.isOn ? dev.color : "#1a202c"}; box-shadow: 0 0 16px ${dev.isOn ? dev.color + "44" : "none"};">
          <span class="rgb-color-label" id="colorName_${dev.id}">${dev.isOn ? colorName : "Đã tắt"}</span>
          <input type="text" class="rgb-hex-input" id="hexInput_${dev.id}" value="${dev.isOn ? dev.color.toUpperCase() : "#000000"}" maxlength="7" onchange="changeRGBHexInput('${dev.id}', this.value)" title="Nhập mã màu Hex tùy ý (VD: #FF5500)" ${!dev.isOn ? "disabled" : ""}>
        </div>

        <div class="custom-picker-row">
          <label class="btn-open-picker" title="Nhấn để chọn bất kỳ màu nào trên bảng màu">
            <span>🎨 Chọn màu trên bảng màu quang phổ</span>
            <input type="color" id="pickerInput_${dev.id}" value="${dev.color}" oninput="changeRGBColorLive('${dev.id}', this.value)" ${!dev.isOn ? "disabled" : ""}>
          </label>
        </div>

        <div class="rgb-channels-mixer">
          <div class="channel-slider-row">
            <span class="channel-tag red"><span>R (Đỏ)</span><span id="val_r_${dev.id}">${r}</span></span>
            <input type="range" class="range-slider-channel red" id="slider_r_${dev.id}" min="0" max="255" value="${r}"
                   oninput="changeRGBChannelLive('${dev.id}', 'r', this.value)" ${!dev.isOn ? "disabled" : ""}>
          </div>
          <div class="channel-slider-row">
            <span class="channel-tag green"><span>G (Lá)</span><span id="val_g_${dev.id}">${g}</span></span>
            <input type="range" class="range-slider-channel green" id="slider_g_${dev.id}" min="0" max="255" value="${g}"
                   oninput="changeRGBChannelLive('${dev.id}', 'g', this.value)" ${!dev.isOn ? "disabled" : ""}>
          </div>
          <div class="channel-slider-row">
            <span class="channel-tag blue"><span>B (Dương)</span><span id="val_b_${dev.id}">${b}</span></span>
            <input type="range" class="range-slider-channel blue" id="slider_b_${dev.id}" min="0" max="255" value="${b}"
                   oninput="changeRGBChannelLive('${dev.id}', 'b', this.value)" ${!dev.isOn ? "disabled" : ""}>
          </div>
        </div>

        <div class="slider-group">
          <div class="slider-header">
            <span>Độ sáng tổng thể</span>
            <span id="val_bright_${dev.id}">${dev.brightness}%</span>
          </div>
          <input type="range" class="range-slider" id="slider_bright_${dev.id}" min="1" max="100" value="${dev.brightness}"
                 oninput="changeRGBBrightness('${dev.id}', this.value)" ${!dev.isOn ? "disabled" : ""}>
        </div>

        <div class="rgb-action-row">
          <button class="btn-save-color ${isUnsaved ? "is-unsaved" : ""}"
                  id="saveBtn_${dev.id}"
                  onclick="saveRGBColor('${dev.id}')"
                  ${!dev.isOn ? "disabled" : ""}>
            ${isUnsaved ? `💾 Lưu màu để phát âm ("${colorName}")` : `✓ Đã lưu màu (${colorName})`}
          </button>
        </div>

        <div class="color-presets-wrapper">
          ${presets
            .map(
              (c) => `
            <div class="preset-chip ${dev.color.toLowerCase() === c.toLowerCase() ? "active" : ""}"
                 data-color="${c}"
                 style="background: ${c};"
                 onclick="changeRGBColor('${dev.id}', '${c}')"
                 title="${getColorName(c)}"></div>
          `,
            )
            .join("")}
        </div>
      </div>
    `;
  }

  // 2. Switch / Relay Device Card
  if (dev.type === "switch") {
    return `
      <div class="device-card ${dev.isOn ? "is-active" : ""}" id="card_${dev.id}">
        <div class="card-header">
          <div class="card-title-group">
            <div class="device-icon-box">
              <svg fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"></path></svg>
            </div>
            <div>
              <div class="device-title">${dev.name}</div>
              <div class="device-subtitle">${roomName} • ${dev.pubTopic}</div>
            </div>
          </div>
          <div class="card-actions">
            <label class="switch">
              <input type="checkbox" ${dev.isOn ? "checked" : ""} onchange="toggleSwitchDevice('${dev.id}', this.checked)">
              <span class="slider"></span>
            </label>
            ${cardActionControls}
          </div>
        </div>
        <div style="display:flex; justify-content:space-between; align-items:center; padding: 8px 12px; background:rgba(255,255,255,0.03); border-radius:var(--radius-md); border: 1px solid var(--border-color);">
          <span style="font-size:0.85rem; color:var(--text-secondary);">Trạng thái</span>
          <span style="font-weight:700; color:${dev.isOn ? "var(--accent-emerald)" : "var(--text-muted)"};">
            ${dev.isOn ? "ĐANG BẬT" : "ĐÃ TẮT"}
          </span>
        </div>
      </div>
    `;
  }

  // 3. Fan Device Card
  if (dev.type === "fan") {
    return `
      <div class="device-card ${dev.isOn ? "is-active" : ""}" id="card_${dev.id}">
        <div class="card-header">
          <div class="card-title-group">
            <div class="device-icon-box" style="${dev.isOn ? "animation: spin 3s linear infinite;" : ""}">
              <svg fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14 10l-2 1m0 0l-2-1m2 1v2.5M20 7l-2 1m2-1l-2-1m2 1v2.5M14 4l-2-1-2 1M4 7l2-1M4 7l2 1M4 7v2.5M12 21l-2-1m2 1l2-1m-2 1v-2.5M6 18l-2-1v-2.5M18 18l2-1v-2.5"></path></svg>
            </div>
            <div>
              <div class="device-title">${dev.name}</div>
              <div class="device-subtitle">${roomName} • ${dev.pubTopic}</div>
            </div>
          </div>
          <div class="card-actions">
            <label class="switch">
              <input type="checkbox" ${dev.isOn ? "checked" : ""} onchange="toggleFanDevice('${dev.id}', this.checked)">
              <span class="slider"></span>
            </label>
            ${cardActionControls}
          </div>
        </div>
        <div class="slider-group">
          <div class="slider-header">
            <span>Cấp độ gió</span>
            <span>${dev.isOn ? `${dev.speed}%` : "Tắt"}</span>
          </div>
          <input type="range" class="range-slider" min="0" max="100" step="25" value="${dev.isOn ? dev.speed : 0}"
                 oninput="changeFanSpeed('${dev.id}', this.value)">
        </div>
      </div>
    `;
  }

  // 4. Curtain Device Card (Mới)
  if (dev.type === "curtain") {
    return `
      <div class="device-card ${dev.position > 0 ? "is-active" : ""}" id="card_${dev.id}">
        <div class="card-header">
          <div class="card-title-group">
            <div class="device-icon-box">
              🪟
            </div>
            <div>
              <div class="device-title">${dev.name}</div>
              <div class="device-subtitle">${roomName} • ${dev.pubTopic}</div>
            </div>
          </div>
          <div class="card-actions">
            ${cardActionControls}
          </div>
        </div>
        <div class="slider-group">
          <div class="slider-header">
            <span>Độ mở rèm</span>
            <span>${dev.position}%</span>
          </div>
          <input type="range" class="range-slider" min="0" max="100" value="${dev.position}"
                 onchange="setCurtainPosition('${dev.id}', this.value)">
        </div>
        <div style="display:flex; gap:8px; margin-top:10px;">
          <button class="push-action-btn" style="flex:1;" onclick="setCurtainPosition('${dev.id}', 100)">Mở hết</button>
          <button class="push-action-btn" style="flex:1; background:var(--bg-secondary);" onclick="setCurtainPosition('${dev.id}', 0)">Đóng rèm</button>
        </div>
      </div>
    `;
  }

  // 5. Sensor Device Card
  if (dev.type === "sensor") {
    return `
      <div class="device-card" id="card_${dev.id}">
        <div class="card-header">
          <div class="card-title-group">
            <div class="device-icon-box" style="color:var(--accent-amber);">
              <svg fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z"></path></svg>
            </div>
            <div>
              <div class="device-title">${dev.name}</div>
              <div class="device-subtitle">${roomName} • ${dev.subTopic}</div>
            </div>
          </div>
          <div class="card-actions">
            ${cardActionControls}
          </div>
        </div>

        <div class="sensor-metrics-grid">
          <div class="sensor-box">
            <div class="sensor-icon">🌡️</div>
            <div>
              <div class="sensor-val" style="color:var(--accent-rose);">${dev.temp ?? 28}°C</div>
              <div class="sensor-lbl">Nhiệt độ phòng</div>
            </div>
          </div>
          <div class="sensor-box">
            <div class="sensor-icon">💧</div>
            <div>
              <div class="sensor-val" style="color:var(--accent-cyan);">${dev.hum ?? 60}%</div>
              <div class="sensor-lbl">Độ ẩm không khí</div>
            </div>
          </div>
        </div>
      </div>
    `;
  }

  // 6. Generic Custom Button / Switch
  return `
    <div class="device-card" id="card_${dev.id}">
      <div class="card-header">
        <div class="card-title-group">
          <div class="device-icon-box">
            <svg fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 6V4m0 2a2 2 0 100 4m0-4a2 2 0 110 4m-6 8a2 2 0 100-4m0 4a2 2 0 110-4m0 4v2m0-6V4m6 6v10m6-2a2 2 0 100-4m0 4a2 2 0 110-4m0 4v2m0-6V4"></path></svg>
          </div>
          <div>
            <div class="device-title">${dev.name}</div>
            <div class="device-subtitle">${roomName} • ${dev.pubTopic}</div>
          </div>
        </div>
        <div class="card-actions">
          ${cardActionControls}
        </div>
      </div>
      <button class="push-action-btn" onclick="triggerCustomButton('${dev.id}')">
        ▶ Kích hoạt lệnh (${dev.onPayload || "TRIGGER"})
      </button>
    </div>
  `;
}

// ==========================================
// 7. DEVICE INTERACTION ACTIONS
// ==========================================

function toggleRGBPower(devId, isChecked) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.isOn = isChecked;
  updateRGBCardDOM(dev);
  sendRGBDeviceUpdate(dev);
}

function changeRGBColor(devId, hexColor) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.color = hexColor;
  dev.savedColor = hexColor;
  dev.isOn = true;
  updateRGBCardDOM(dev);
  sendRGBDeviceUpdate(dev, true);
  showToast(`Đã chọn màu: "${getColorName(hexColor)}"`, "info");
}

function changeRGBColorLive(devId, hexColor) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.color = hexColor;
  dev.isOn = true;
  updateRGBCardDOM(dev);
  debouncedSendRGB(dev);
}

function changeRGBChannelLive(devId, channel, value) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  const current = hexToRgb(dev.color);
  current[channel] = parseInt(value) || 0;
  dev.color = rgbToHex(current.r, current.g, current.b);
  dev.isOn = true;
  updateRGBCardDOM(dev);
  debouncedSendRGB(dev);
}

function changeRGBHexInput(devId, inputHex) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  let cleanHex = inputHex.trim();
  if (!cleanHex.startsWith("#")) cleanHex = "#" + cleanHex;
  if (!/^#[0-9A-Fa-f]{6}$/.test(cleanHex)) {
    showToast("Mã màu Hex không hợp lệ! VD: #FF5500", "error");
    updateRGBCardDOM(dev);
    return;
  }
  dev.color = cleanHex;
  dev.savedColor = cleanHex;
  dev.isOn = true;
  updateRGBCardDOM(dev);
  sendRGBDeviceUpdate(dev, true);
  showToast("Đã áp dụng mã màu: " + cleanHex.toUpperCase(), "success");
}

function saveRGBColor(devId) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;

  dev.savedColor = dev.color;

  sendRGBDeviceUpdate(dev, true);
  updateRGBCardDOM(dev);
  showToast(
    `Đã lưu màu và gửi giọng nói: "${getColorName(dev.color)}"`,
    "success",
  );
}

function updateRGBCardDOM(dev) {
  const { r, g, b } = hexToRgb(dev.color);
  const colorName = getColorName(dev.color);
  const isUnsaved =
    dev.color.toLowerCase() !== (dev.savedColor || "").toLowerCase();

  const card = document.getElementById(`card_${dev.id}`);
  if (card) {
    card.classList.toggle("is-active", dev.isOn);
    const iconBox = card.querySelector(".device-icon-box");
    if (iconBox) {
      iconBox.style.background = dev.isOn ? `${dev.color}22` : "";
      iconBox.style.color = dev.isOn ? dev.color : "";
    }
  }

  const switchInput = document.getElementById(`switch_${dev.id}`);
  if (switchInput) switchInput.checked = dev.isOn;

  const banner = document.getElementById(`banner_${dev.id}`);
  if (banner) {
    banner.style.background = dev.isOn ? dev.color : "#1a202c";
    banner.style.boxShadow = `0 0 16px ${dev.isOn ? dev.color + "44" : "none"}`;
  }

  const colorNameEl = document.getElementById(`colorName_${dev.id}`);
  if (colorNameEl) colorNameEl.textContent = dev.isOn ? colorName : "Đã tắt";

  const hexInput = document.getElementById(`hexInput_${dev.id}`);
  if (hexInput) {
    hexInput.value = dev.isOn ? dev.color.toUpperCase() : "#000000";
    hexInput.disabled = !dev.isOn;
  }

  const pickerInput = document.getElementById(`pickerInput_${dev.id}`);
  if (pickerInput) {
    pickerInput.value = dev.color;
    pickerInput.disabled = !dev.isOn;
  }

  const valR = document.getElementById(`val_r_${dev.id}`);
  if (valR) valR.textContent = r;
  const sliderR = document.getElementById(`slider_r_${dev.id}`);
  if (sliderR) {
    sliderR.value = r;
    sliderR.disabled = !dev.isOn;
  }

  const valG = document.getElementById(`val_g_${dev.id}`);
  if (valG) valG.textContent = g;
  const sliderG = document.getElementById(`slider_g_${dev.id}`);
  if (sliderG) {
    sliderG.value = g;
    sliderG.disabled = !dev.isOn;
  }

  const valB = document.getElementById(`val_b_${dev.id}`);
  if (valB) valB.textContent = b;
  const sliderB = document.getElementById(`slider_b_${dev.id}`);
  if (sliderB) {
    sliderB.value = b;
    sliderB.disabled = !dev.isOn;
  }

  const valBright = document.getElementById(`val_bright_${dev.id}`);
  if (valBright) valBright.textContent = dev.brightness + "%";
  const sliderBright = document.getElementById(`slider_bright_${dev.id}`);
  if (sliderBright) {
    sliderBright.value = dev.brightness;
    sliderBright.disabled = !dev.isOn;
  }

  const saveBtn = document.getElementById(`saveBtn_${dev.id}`);
  if (saveBtn) {
    saveBtn.classList.toggle("is-unsaved", isUnsaved);
    saveBtn.disabled = !dev.isOn;
    saveBtn.innerHTML = isUnsaved
      ? `💾 Lưu màu để phát âm ("${colorName}")`
      : `✓ Đã lưu màu (${colorName})`;
  }

  if (card) {
    const chips = card.querySelectorAll(".preset-chip");
    chips.forEach((chip) => {
      const chipColor = chip.getAttribute("data-color");
      if (chipColor) {
        chip.classList.toggle(
          "active",
          dev.color.toLowerCase() === chipColor.toLowerCase(),
        );
      }
    });
  }
}

function changeRGBBrightness(devId, val) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.brightness = parseInt(val);
  debouncedSendRGB(dev);

  const valBright = document.getElementById(`val_bright_${devId}`);
  if (valBright) valBright.textContent = val + "%";
}

function toggleSwitchDevice(devId, isChecked) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.isOn = isChecked;
  const payload = isChecked ? dev.onPayload || "ON" : dev.offPayload || "OFF";
  publishMQTT(dev.pubTopic || "esp32/web", payload);
  saveState();
  renderDevices();
}

function toggleFanDevice(devId, isChecked) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.isOn = isChecked;
  const payload = isChecked ? `fan_on_${dev.speed || 25}` : "fan_off";
  publishMQTT(dev.pubTopic || "esp32/fan", payload);
  saveState();
  renderDevices();
}

function changeFanSpeed(devId, speed) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  const selectedSpeed = parseInt(speed, 10);
  if (selectedSpeed === 0) {
    toggleFanDevice(devId, false);
    return;
  }
  dev.speed = selectedSpeed;
  dev.isOn = true;
  publishMQTT(dev.pubTopic || "esp32/fan", `fan_on_${dev.speed}`);
  saveState();
  renderDevices();
}

function setCurtainPosition(devId, pos) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  dev.position = parseInt(pos);
  publishMQTT(dev.pubTopic || "esp32/curtain", String(dev.position));
  saveState();
  renderDevices();
  showToast(`Đã điều chỉnh rèm: ${dev.position}%`, "info");
}

function triggerCustomButton(devId) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;
  publishMQTT(dev.pubTopic, dev.onPayload || "TRIGGER");
  showToast(`Đã gửi lệnh tới ${dev.name}`, "info");
}

function deleteDevice(devId) {
  if (confirm("Bạn có chắc chắn muốn xóa thiết bị này không?")) {
    state.devices = state.devices.filter((d) => d.id !== devId);
    saveState();
    renderDevices();
    showToast("Đã xóa thiết bị", "info");
  }
}

// ==========================================
// 8. EDIT DEVICE MODAL HANDLERS
// ==========================================

function openEditDeviceModal(devId) {
  const dev = state.devices.find((d) => d.id === devId);
  if (!dev) return;

  state.editingDeviceId = devId;

  const nameInput = document.getElementById("editDevName");
  const roomSelect = document.getElementById("editDevRoom");
  const pubInput = document.getElementById("editDevPub");
  const subInput = document.getElementById("editDevSub");
  const payloadInput = document.getElementById("editDevPayload");

  if (nameInput) nameInput.value = dev.name;
  if (pubInput) pubInput.value = dev.pubTopic || "";
  if (subInput) subInput.value = dev.subTopic || "";
  if (payloadInput) payloadInput.value = dev.onPayload || "";

  if (roomSelect) {
    roomSelect.innerHTML = `<option value="">Không gán phòng</option>${state.rooms
      .filter((r) => r.id !== "all")
      .map(
        (r) =>
          `<option value="${r.id}" ${r.id === dev.room ? "selected" : ""}>${r.name}</option>`,
      )
      .join("")}`;
  }

  openModal("editDeviceModal");
}

function handleEditDeviceSubmit(e) {
  e.preventDefault();
  if (!state.editingDeviceId) return;

  const dev = state.devices.find((d) => d.id === state.editingDeviceId);
  if (!dev) return;

  const name = document.getElementById("editDevName").value.trim();
  const room = document.getElementById("editDevRoom").value;
  const pubTopic = document.getElementById("editDevPub").value.trim();
  const subTopic = document.getElementById("editDevSub").value.trim();
  const onPayload = document.getElementById("editDevPayload").value.trim();

  if (!name || !pubTopic) {
    showToast("Vui lòng điền đầy đủ tên và Topic gửi MQTT!", "error");
    return;
  }

  dev.name = name;
  dev.room = room;
  dev.pubTopic = pubTopic;
  dev.subTopic = subTopic;
  if (onPayload) dev.onPayload = onPayload;

  saveState();
  subscribeAllTopics();
  renderDevices();
  closeModal("editDeviceModal");
  showToast("Đã cập nhật cấu hình thiết bị!", "success");
}

// ==========================================
// 9. QUICK SCENES EXECUTION
// ==========================================

function activateScene(sceneName) {
  addLog("SCENE", `Kích hoạt kịch bản: ${sceneName}`, "out");

  if (sceneName === "home") {
    state.devices.forEach((d) => {
      if (d.type === "switch") {
        d.isOn = true;
        publishMQTT(d.pubTopic, d.onPayload || "ON");
      }
      if (d.type === "rgb") {
        d.isOn = true;
        d.color = "#fef08a"; // Trắng ấm
        d.brightness = 100;
        sendRGBDeviceUpdate(d);
      }
      if (d.type === "curtain") {
        setCurtainPosition(d.id, 100);
      }
    });
    showToast("Đã kích hoạt chế độ Về Nhà 🏡", "success");
  } else if (sceneName === "sleep") {
    state.devices.forEach((d) => {
      if (d.type === "switch") {
        d.isOn = false;
        publishMQTT(d.pubTopic, d.offPayload || "OFF");
      }
      if (d.type === "rgb") {
        d.isOn = true;
        d.color = "#a855f7"; // Tím dịu
        d.brightness = 15;
        sendRGBDeviceUpdate(d);
      }
      if (d.type === "curtain") {
        setCurtainPosition(d.id, 0);
      }
      if (d.type === "fan") {
        d.isOn = true;
        d.speed = 25;
        publishMQTT(d.pubTopic, "fan_on_25");
      }
    });
    showToast("Đã kích hoạt chế độ Đi Ngủ 🌙", "success");
  } else if (sceneName === "relax") {
    state.devices.forEach((d) => {
      if (d.type === "rgb") {
        d.isOn = true;
        d.color = "#06b6d4";
        d.brightness = 80;
        sendRGBDeviceUpdate(d);
      }
    });
    showToast("Đã kích hoạt chế độ Thư Giãn ☕", "success");
  } else if (sceneName === "all_off") {
    state.devices.forEach((d) => {
      if (d.type === "switch") {
        d.isOn = false;
        publishMQTT(d.pubTopic, d.offPayload || "OFF");
      }
      if (d.type === "rgb") {
        d.isOn = false;
        sendRGBDeviceUpdate(d);
      }
      if (d.type === "fan") {
        d.isOn = false;
        publishMQTT(d.pubTopic, "fan_off");
      }
    });
    showToast("Đã tắt toàn bộ thiết bị ⚡", "info");
  }

  saveState();
  renderDevices();
}

// ==========================================
// 10. MODAL HANDLERS & BACKUP / RESTORE
// ==========================================

function openModal(modalId) {
  const modal = document.getElementById(modalId);
  if (modal) modal.classList.add("active");
}

function closeModal(modalId) {
  const modal = document.getElementById(modalId);
  if (modal) modal.classList.remove("active");
}

function handleAddDeviceSubmit(e) {
  e.preventDefault();
  const name = document.getElementById("newDevName").value.trim();
  const type = document.getElementById("newDevType").value;
  const room = document.getElementById("newDevRoom").value;
  const pubTopic = document.getElementById("newDevPub").value.trim();
  const subTopic = document.getElementById("newDevSub").value.trim();
  const onPayload =
    document.getElementById("newDevPayload").value.trim() || "ON";

  if (!name || !pubTopic) {
    showToast("Vui lòng nhập tên và Topic gửi MQTT!", "error");
    return;
  }

  const newDevice = {
    id: "dev_" + Date.now(),
    name,
    type,
    room,
    pubTopic,
    subTopic,
    isOn: false,
    onPayload,
    offPayload: "OFF",
    color: "#3b82f6",
    brightness: 100,
    speed: type === "fan" ? 25 : 1,
    position: 0,
    temp: 28,
    hum: 60,
  };

  state.devices.push(newDevice);
  saveState();
  subscribeAllTopics();
  renderDevices();
  closeModal("addDeviceModal");
  showToast(`Đã thêm thiết bị "${name}" thành công!`, "success");

  document.getElementById("addDeviceForm").reset();
}

function handleAddRoomSubmit(e) {
  e.preventDefault();
  const name = document.getElementById("newRoomName").value.trim();
  if (!name) return;

  const id = "room_" + Date.now();
  state.rooms.push({ id, name });
  saveState();
  renderRooms();
  updateRoomSelectOptions();
  closeModal("addRoomModal");
  showToast(`Đã thêm phòng "${name}"!`, "success");
  document.getElementById("newRoomName").value = "";
}

function updateRoomSelectOptions() {
  const select = document.getElementById("newDevRoom");
  if (!select) return;
  select.innerHTML = `<option value="">Không gán phòng</option>${state.rooms
    .filter((r) => r.id !== "all")
    .map((r) => `<option value="${r.id}">${r.name}</option>`)
    .join("")}`;
}

function handleSaveBrokerConfig() {
  const url = document.getElementById("brokerUrlInput").value.trim();
  if (!url) return;
  state.brokerUrl = url;
  localStorage.setItem("mqttBrokerUrl", url);
  initMQTT();
  closeModal("mqttModal");
}

// ==========================================
// 11. CONSOLE LOG MANAGEMENT & UTILITIES
// ==========================================

function clearLogs() {
  const consoleBody = document.getElementById("consoleBody");
  if (consoleBody) {
    consoleBody.innerHTML = "";
    showToast("Đã xóa nhật ký MQTT", "info");
  }
}

function toggleTheme() {
  state.theme = state.theme === "dark" ? "light" : "dark";
  document.documentElement.setAttribute("data-theme", state.theme);
  localStorage.setItem("smarthome_theme", state.theme);
  const icon = document.getElementById("themeIcon");
  if (icon) icon.textContent = state.theme === "dark" ? "🌙" : "☀️";
}

function showToast(msg, type = "info") {
  let container = document.getElementById("toastContainer");
  if (!container) {
    container = document.createElement("div");
    container.id = "toastContainer";
    container.className = "toast-container";
    document.body.appendChild(container);
  }

  const toast = document.createElement("div");
  toast.className = `toast ${type}`;
  toast.innerHTML = `<span>${type === "success" ? "✓" : type === "error" ? "✕" : "ℹ"}</span> <span>${msg}</span>`;
  container.appendChild(toast);

  setTimeout(() => {
    toast.remove();
  }, 4000);
}

function addLog(topic, msg, dir = "in") {
  const consoleBody = document.getElementById("consoleBody");
  if (!consoleBody) return;

  const timeStr = new Date().toLocaleTimeString("vi-VN");
  const entry = document.createElement("div");
  entry.className = "log-entry";
  entry.innerHTML = `
    <span class="log-time">[${timeStr}]</span>
    <span class="${dir === "out" ? "log-out" : "log-in"}">${dir === "out" ? "📤" : "📥"}</span>
    <span class="log-topic">[${topic}]</span>
    <span class="log-msg">${typeof msg === "object" ? JSON.stringify(msg) : msg}</span>
  `;

  consoleBody.prepend(entry);

  if (consoleBody.children.length > 50) {
    consoleBody.removeChild(consoleBody.lastChild);
  }
}

function toggleConsole() {
  const body = document.getElementById("consoleBody");
  const arrow = document.getElementById("consoleToggleArrow");
  if (body) {
    body.classList.toggle("collapsed");
    if (arrow)
      arrow.textContent = body.classList.contains("collapsed")
        ? "▲ Mở"
        : "▼ Thu gọn";
  }
}

function updateClock() {
  const clockEl = document.getElementById("currentClock");
  if (clockEl) {
    const now = new Date();
    clockEl.textContent =
      now.toLocaleTimeString("vi-VN", { hour: "2-digit", minute: "2-digit" }) +
      " • " +
      now.toLocaleDateString("vi-VN", {
        weekday: "short",
        day: "2-digit",
        month: "2-digit",
      });
  }
}

// ==========================================
// 12. INITIALIZATION & EVENT LISTENERS
// ==========================================

window.addEventListener("DOMContentLoaded", async () => {
  try {
    state.devices = await loadDevices();
    await saveState();
  } catch (error) {
    console.error("Không thể khởi tạo cơ sở dữ liệu thiết bị:", error);
    showToast("Không thể tải thiết bị từ cơ sở dữ liệu!", "error");
  }

  // Set theme
  document.documentElement.setAttribute("data-theme", state.theme);
  const themeIcon = document.getElementById("themeIcon");
  if (themeIcon) themeIcon.textContent = state.theme === "dark" ? "🌙" : "☀️";

  // Broker input value
  const brokerInput = document.getElementById("brokerUrlInput");
  if (brokerInput) brokerInput.value = state.brokerUrl;

  // Search input listener
  const searchInput = document.getElementById("deviceSearchInput");
  if (searchInput) {
    searchInput.addEventListener("input", (e) =>
      handleSearchDevices(e.target.value),
    );
  }

  // Backdrop click listener cho Modals
  document.querySelectorAll(".modal").forEach((modal) => {
    modal.addEventListener("click", (e) => {
      if (e.target === modal) closeModal(modal.id);
    });
  });

  // Render Dashboard
  renderRooms();
  updateRoomSelectOptions();
  renderDevices();

  // Clock Timer
  updateClock();
  setInterval(updateClock, 1000);

  // Connect MQTT Engine
  initMQTT();
});
