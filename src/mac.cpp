#include "mac.h"
#include <ArduinoJson.h>
#include <sys/time.h>

MacStats MAC = {};
volatile int shotRequest = -1, rowsY = -1, rowsN = 0;
volatile bool shotDone = false;

static char line[3072];
static size_t len = 0;

static uint8_t readTop(JsonArrayConst arr, TopProc *out) {
  uint8_t n = 0;
  for (JsonArrayConst row : arr) {
    if (n >= TOP_N) break;
    strlcpy(out[n].name, row[0] | "?", sizeof(out[n].name));
    out[n].value = row[1] | 0.0f;
    n++;
  }
  return n;
}

static void handleLine() {
  JsonDocument doc;
  if (deserializeJson(doc, line, len) || !doc["mac"].is<int>()) return;

  strlcpy(MAC.host, doc["host"] | "Mac", sizeof(MAC.host));

  MAC.cpu = doc["cpu"] | 0.0f;
  MAC.cpuUser = doc["cu"] | 0.0f;
  MAC.cpuSys = doc["cs"] | 0.0f;
  for (int i = 0; i < 3; i++) MAC.load[i] = doc["la"][i] | 0.0f;
  MAC.nCores = 0;
  for (JsonVariantConst c : doc["cores"].as<JsonArrayConst>())
    if (MAC.nCores < MAX_CORES) MAC.cores[MAC.nCores++] = c.as<int>();
  MAC.pCores = doc["pc"] | 0;
  MAC.nTopCpu = readTop(doc["tc"].as<JsonArrayConst>(), MAC.topCpu);
  MAC.temp = doc["tmp"] | -1.0f;

  MAC.gpu = doc["gpu"] | -1.0f;
  MAC.gpuRender = doc["gR"] | -1.0f;
  MAC.gpuTiler = doc["gT"] | -1.0f;
  MAC.gpuMem = doc["gpuM"] | 0.0f;
  MAC.gpuAlloc = doc["gA"] | 0.0f;

  MAC.ram = doc["ram"] | 0.0f;
  MAC.ramUsed = doc["ramU"] | 0.0f;
  MAC.ramTotal = doc["ramT"] | 0.0f;
  MAC.memPressure = doc["mp"] | -1;
  MAC.memLevel = doc["mpl"] | 0;
  MAC.memApp = doc["mApp"] | -1.0f;
  MAC.memWired = doc["mWir"] | 0.0f;
  MAC.memComp = doc["mCmp"] | 0.0f;
  MAC.memCached = doc["mCac"] | 0.0f;
  MAC.swap = doc["swp"] | 0.0f;
  MAC.swapTotal = doc["swT"] | 0.0f;
  MAC.nTopMem = readTop(doc["tm"].as<JsonArrayConst>(), MAC.topMem);

  MAC.rx = doc["rx"] | 0.0f;
  MAC.tx = doc["tx"] | 0.0f;
  MAC.rxTotal = doc["rxT"] | 0.0f;
  MAC.txTotal = doc["txT"] | 0.0f;
  if (doc["if"].is<const char *>()) {  // refreshed every 10 s
    strlcpy(MAC.ifName, doc["if"] | "", sizeof(MAC.ifName));
    strlcpy(MAC.ifType, doc["it"] | "", sizeof(MAC.ifType));
    strlcpy(MAC.ip, doc["ip"] | "", sizeof(MAC.ip));
  }

  JsonObjectConst inf = doc["info"];
  if (!inf.isNull()) {
    MacInfo &m = MAC.info;
    strlcpy(m.model, inf["mdl"] | "Mac", sizeof(m.model));
    strlcpy(m.modelId, inf["id"] | "", sizeof(m.modelId));
    strlcpy(m.chip, inf["chip"] | "", sizeof(m.chip));
    strlcpy(m.mem, inf["mem"] | "", sizeof(m.mem));
    strlcpy(m.os, inf["os"] | "", sizeof(m.os));
    strlcpy(m.metal, inf["metal"] | "", sizeof(m.metal));
    strlcpy(m.display, inf["disp"] | "", sizeof(m.display));
    strlcpy(m.battery, inf["bHl"] | "", sizeof(m.battery));
    m.cpuN = inf["cpuN"] | 0;
    m.cpuP = inf["cpuP"] | 0;
    m.cpuE = inf["cpuE"] | 0;
    m.gpuN = inf["gpuN"] | 0;
    m.batteryCycles = inf["bCyc"] | -1;
    const char *maxCap = inf["bMax"] | "";
    if (*maxCap && m.battery[0]) {  // "Good, 96%"
      strlcat(m.battery, ", ", sizeof(m.battery));
      strlcat(m.battery, maxCap, sizeof(m.battery));
    }
    m.boot = inf["boot"] | 0L;
    m.any = true;
  }

  JsonObjectConst cl = doc["cl"];
  if (!cl.isNull()) {
    ClaudeStats &c = MAC.claude;
    JsonArrayConst td = cl["td"], bk = cl["bk"];
    c.tdIn = td[0] | 0.0f;
    c.tdOut = td[1] | 0.0f;
    c.tdCacheRead = td[2] | 0.0f;
    c.tdCacheWrite = td[3] | 0.0f;
    c.tdReq = td[4] | 0;
    c.bkStart = bk[0] | 0L;
    c.bkEnd = bk[1] | 0L;
    c.bkIn = bk[2] | 0.0f;
    c.bkOut = bk[3] | 0.0f;
    c.bkCacheRead = bk[4] | 0.0f;
    c.bkCacheWrite = bk[5] | 0.0f;
    c.bkReq = bk[6] | 0;
    c.bkActive = bk[7] | 0;
    c.burn = cl["br"] | 0;
    int i = 0;
    for (JsonVariantConst v : cl["pm"].as<JsonArrayConst>())
      if (i < 60) c.perMin[i++] = v.as<uint32_t>();
    c.nModels = 0;
    for (JsonArrayConst m : cl["md"].as<JsonArrayConst>()) {
      if (c.nModels >= 3) break;
      strlcpy(c.models[c.nModels].name, m[0] | "?", sizeof(c.models[0].name));
      c.models[c.nModels].out = m[1] | 0.0f;
      c.models[c.nModels].req = m[2] | 0;
      c.nModels++;
    }
    c.nProjects = 0;
    for (JsonArrayConst p : cl["pj"].as<JsonArrayConst>()) {
      if (c.nProjects >= 4) break;
      strlcpy(c.projects[c.nProjects].name, p[0] | "?", sizeof(c.projects[0].name));
      c.projects[c.nProjects].tokens = p[1] | 0.0f;
      c.nProjects++;
    }
    c.any = true;
  }

  MAC.bat = doc["bat"] | -1;
  MAC.charging = doc["chg"] | 0;
  MAC.onAC = doc["ac"] | 0;
  MAC.diskUsed = doc["dU"] | 0.0f;
  MAC.diskTotal = doc["dT"] | 0.0f;

  MAC.hCpu[MAC.head] = MAC.cpu;
  MAC.hGpu[MAC.head] = max(MAC.gpu, 0.0f);
  MAC.hGpuMem[MAC.head] = MAC.gpuMem;
  MAC.hMp[MAC.head] = MAC.memPressure >= 0 ? MAC.memPressure : MAC.ram;
  MAC.hRx[MAC.head] = MAC.rx;
  MAC.hTx[MAC.head] = MAC.tx;
  MAC.head = (MAC.head + 1) % MAC_HIST;
  if (MAC.count < MAC_HIST) MAC.count++;
  MAC.lastMs = millis();
  MAC.any = true;

  // The board has no network: the clock follows the Mac's time and timezone.
  long t = doc["t"] | 0L;
  if (t > 1700000000) {
    struct timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
    MAC.tzOffset = doc["tz"] | 0L;
    MAC.timeSet = true;
  }
}

bool macPoll() {
  bool got = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[len] = 0;
      if (len && line[0] == '{') {
        handleLine();
        got = true;
      } else if (len > 5 && !strncmp(line, "SHOT ", 5)) {
        shotRequest = atoi(line + 5);
      } else if (len > 5 && !strncmp(line, "ROWS ", 5)) {
        int y = 0, n = 0;
        if (sscanf(line + 5, "%d %d", &y, &n) == 2) {
          rowsN = n;
          rowsY = y;
        }
      } else if (!strcmp(line, "DONE")) {
        shotDone = true;
      }
      len = 0;
    } else if (c != '\r' && len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
  return got;
}

bool macLive() { return MAC.any && millis() - MAC.lastMs < 5000; }
