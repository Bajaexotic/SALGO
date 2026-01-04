// -------------------- Snapshot Unified (Socket Streamer)
// FIXED: Header order and Preprocessor definitions to prevent conflicts

// 1. CRITICAL MACROS: Must be defined before ANY includes
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// 2. CRITICAL INCLUDE ORDER: winsock2 must be first
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

// 3. Sierra Chart Include
#include "sierrachart.h"

// 4. Link the library
#pragma comment(lib, "ws2_32.lib")

SCDLLName("Snapshot Unified — Socket Streamer")

// Global Socket Variables
SOCKET udpSocket = INVALID_SOCKET;
struct sockaddr_in serverAddr;
bool isSocketInitialized = false;

// Helper: Initialize the Socket
void InitSocket(SCStudyInterfaceRef sc, int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        sc.AddMessageToLog("WSAStartup failed", 1);
        return;
    }

    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        sc.AddMessageToLog("Socket creation failed", 1);
        WSACleanup();
        return;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    isSocketInitialized = true;
    
    SCString msg;
    msg.Format("UDP Socket Initialized on Port %d", port);
    sc.AddMessageToLog(msg, 1);
}

SCSFExport scsf_Snapshot_Unified_Socket(SCStudyInterfaceRef sc)
{
    // 1. Configuration
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Snapshot Unified (Socket Stream)";
        sc.StudyDescription = "Streams Last, High, Low, VWAP, POC, Delta, ATR1, Volatility, and ATR2 via UDP.";
        sc.AutoLoop         = 1; 
        sc.UpdateAlways     = 1;

        // Settings
        sc.Input[0].Name = "Target Port";   sc.Input[0].SetInt(5005);
        sc.Input[1].Name = "Throttle (ms)"; sc.Input[1].SetInt(50); 

        // Standard Studies
        sc.Input[2].Name = "POC Study ID (VbP)"; sc.Input[2].SetInt(2);
        sc.Input[3].Name = "POC Subgraph #";     sc.Input[3].SetInt(2); 
        sc.Input[4].Name = "VWAP Study ID";      sc.Input[4].SetInt(5); 
        sc.Input[5].Name = "VWAP Subgraph #";    sc.Input[5].SetInt(1); 
        sc.Input[6].Name = "CumDelta Study ID";  sc.Input[6].SetInt(3);
        sc.Input[7].Name = "CumDelta Subgraph #";sc.Input[7].SetInt(4); 

        // --- CUSTOM STUDIES ---
        
        // ATR 1 (Existing)
        sc.Input[8].Name = "ATR 1 Study ID";       sc.Input[8].SetInt(7);
        sc.Input[9].Name = "ATR 1 Subgraph #";     sc.Input[9].SetInt(1);
        
        // Volatility (Existing)
        sc.Input[10].Name = "Volatility Study ID"; sc.Input[10].SetInt(6);
        sc.Input[11].Name = "Volatility SG #";     sc.Input[11].SetInt(1);
        
        // ATR 2 (New)
        sc.Input[12].Name = "ATR 2 Study ID";      sc.Input[12].SetInt(8);
        sc.Input[13].Name = "ATR 2 Subgraph #";    sc.Input[13].SetInt(1);

        return;
    }

    // 2. Persistent Logic (Init Socket)
    if (!isSocketInitialized) {
        InitSocket(sc, sc.Input[0].GetInt());
    }

    // 3. Only run on the most recent bar (Live)
    if (sc.Index < sc.ArraySize - 1) return;

    // 4. Throttle Check
    int& lastSendTime = sc.GetPersistentInt(0);
    int currentTime = GetTickCount(); 
    if (currentTime - lastSendTime < sc.Input[1].GetInt()) return;
    lastSendTime = currentTime;

    // 5. Gather Data
    const int idx = sc.Index;
    float last = sc.LastTradePrice;
    float high = sc.High[idx];
    float low  = sc.Low[idx];

    // Map Inputs
    const int vbp_id   = sc.Input[2].GetInt();  const int poc_ui   = sc.Input[3].GetInt();
    const int vwap_id  = sc.Input[4].GetInt();  const int vwap_ui  = sc.Input[5].GetInt();
    const int cd_id    = sc.Input[6].GetInt();  const int cd_ui    = sc.Input[7].GetInt();
    const int atr1_id  = sc.Input[8].GetInt();  const int atr1_ui  = sc.Input[9].GetInt();
    const int vol_id   = sc.Input[10].GetInt(); const int vol_ui   = sc.Input[11].GetInt();
    const int atr2_id  = sc.Input[12].GetInt(); const int atr2_ui  = sc.Input[13].GetInt(); // New

    // Adjust 1-based inputs to 0-based indices
    const int poc_idx  = poc_ui  > 0 ? poc_ui  - 1 : 0;
    const int vwap_idx = vwap_ui > 0 ? vwap_ui - 1 : 0;
    const int cd_idx   = cd_ui   > 0 ? cd_ui   - 1 : 0;
    const int atr1_idx = atr1_ui > 0 ? atr1_ui - 1 : 0;
    const int vol_idx  = vol_ui  > 0 ? vol_ui  - 1 : 0;
    const int atr2_idx = atr2_ui > 0 ? atr2_ui - 1 : 0; // New

    // Retrieve Arrays
    SCFloatArray poc_arr, vwap_arr, cd_arr, atr1_arr, vol_arr, atr2_arr;
    
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, vbp_id,   poc_idx,  poc_arr);
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, vwap_id,  vwap_idx, vwap_arr);
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, cd_id,    cd_idx,   cd_arr);
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, atr1_id,  atr1_idx, atr1_arr);
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, vol_id,   vol_idx,  vol_arr);
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, atr2_id,  atr2_idx, atr2_arr); // New

    // Safety Check (Ensure all arrays are valid)
    if (poc_arr.GetArraySize() == 0 || vwap_arr.GetArraySize() == 0 || cd_arr.GetArraySize() == 0 || 
        atr1_arr.GetArraySize() == 0 || vol_arr.GetArraySize() == 0 || atr2_arr.GetArraySize() == 0) return;

    float poc  = poc_arr[idx];
    float vwap = vwap_arr[idx];
    float cd   = cd_arr[idx];
    float atr1 = atr1_arr[idx];
    float vol  = vol_arr[idx];
    float atr2 = atr2_arr[idx]; // New

    // 6. Format JSON
    char buffer[1024];
    const char* symbol = sc.Symbol.GetChars();

    // Sanitize infinite values
    if (!std::isfinite(poc)) poc = 0;
    if (!std::isfinite(vwap)) vwap = 0;
    if (!std::isfinite(atr1)) atr1 = 0;
    if (!std::isfinite(vol)) vol = 0;
    if (!std::isfinite(atr2)) atr2 = 0;

    // Send formatted JSON
    sprintf_s(buffer, sizeof(buffer),
        "{\"sym\": \"%s\", \"last\": %.2f, \"high\": %.2f, \"low\": %.2f, \"vwap\": %.2f, \"poc\": %.2f, \"cd\": %.0f, \"atr1\": %.2f, \"vol\": %.2f, \"atr2\": %.2f}", 
        symbol, last, high, low, vwap, poc, cd, atr1, vol, atr2);

    // 7. Send via UDP
    if (isSocketInitialized) {
        sendto(udpSocket, buffer, (int)strlen(buffer), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    }
}