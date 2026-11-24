// Mac stats received over USB serial from mac/mac_monitor.py (one JSON line per second).
#pragma once
#include <Arduino.h>

#define MAC_HIST  150  // seconds of history kept (detail pages show all of it)
#define OVERVIEW  60   // seconds shown on the overview graphs
#define MAX_CORES 16
#define TOP_N     5

struct TopProc {
  char name[28];
  float value;  // CPU % or memory GB
};

// Claude Code token usage (from Claude Code's local logs on the Mac; refreshed every 10 s)
struct ClaudeStats {
  bool any;
  float tdIn, tdOut, tdCacheRead, tdCacheWrite;  // today
  int tdReq;
  long bkStart, bkEnd;  // current 5-hour window (unix time)
  float bkIn, bkOut, bkCacheRead, bkCacheWrite;
  int bkReq, burn;  // requests, output tokens/min in the window
  bool bkActive;
  uint32_t perMin[60];  // output tokens per minute, last hour (oldest first)
  struct { char name[16]; float out; int req; } models[3];
  uint8_t nModels;
  struct { char name[20]; float tokens; } projects[4];
  uint8_t nProjects;
};

struct MacStats {
  char host[32];
  // CPU
  float cpu, cpuUser, cpuSys, load[3];
  uint8_t cores[MAX_CORES], nCores, pCores;  // per-core %, first pCores are performance cores
  TopProc topCpu[TOP_N];
  uint8_t nTopCpu;
  float temp;  // CPU die temp in C (<= 0 if unknown)
  // GPU
  float gpu, gpuRender, gpuTiler, gpuMem, gpuAlloc;  // % (gpu < 0 = unknown), GB
  // Memory
  float ram, ramUsed, ramTotal;  // psutil view: %, GB, GB
  int memPressure, memLevel;     // pressure % (< 0 = unknown); level 0 normal, 1 warning, 2 critical
  float memApp, memWired, memComp, memCached, swap, swapTotal;  // GB, like Activity Monitor
  TopProc topMem[TOP_N];
  uint8_t nTopMem;
  // Network
  float rx, tx;            // bytes/s
  float rxTotal, txTotal;  // GB since the Mac booted
  char ifName[12], ifType[20], ip[16];
  // Power / disk
  int bat;  // % (-1 = no battery)
  bool charging, onAC;
  float diskUsed, diskTotal;  // GB
  // Claude Code
  ClaudeStats claude;
  // Clock
  long tzOffset;  // Mac's UTC offset in seconds
  bool timeSet;   // clock has been set from the Mac
  // Bookkeeping + history ring buffers (head = next write slot)
  uint32_t lastMs;
  bool any;
  float hCpu[MAC_HIST], hGpu[MAC_HIST], hGpuMem[MAC_HIST], hMp[MAC_HIST], hRx[MAC_HIST], hTx[MAC_HIST];
  uint8_t head, count;
};

extern MacStats MAC;

// Reads pending serial bytes; returns true when a new sample was parsed.
bool macPoll();
// True if a sample arrived in the last few seconds.
bool macLive();
// Screenshot requests from the Mac (main.cpp handles and clears them):
//   "SHOT <page>"  render that page and hold it   -> shotRequest
//   "ROWS <y> <n>" send n rows starting at y       -> rowsY / rowsN
//   "DONE"         back to normal                  -> shotDone
extern volatile int shotRequest, rowsY, rowsN;
extern volatile bool shotDone;
