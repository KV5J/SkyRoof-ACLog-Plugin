// LoggerInterface.cpp  v4
// SkyRoof -> N3FJP AC Log bridge plugin for KV5J
//
// v2: fills AC Log's entry form through the TCP API instead of adding a raw
// ADIF record, so AC Log does its callbook lookup (name, QTH, grid, etc.)
// before logging:
//   CLEAR -> Call -> CALLTAB (lookup) -> wait -> band/mode + SkyRoof fields -> ENTER
//
// SkyRoof loads LoggerInterface.dll from its program folder and calls:
//   void  Init()
//   char* SaveQso(const char* json)    -> "" = success, otherwise error text
//   char* Augment(const char* json)    -> returns QSO json (pass-through here)
//   char* GetStatus(const char* json)  -> returns QSO json (pass-through here)
// Returned strings are freed by .NET with CoTaskMemFree, so they are
// allocated with CoTaskMemAlloc.
//
// Every QSO is first appended to SkyRoof's Adif log, then
// logged to AC Log on a background thread so SkyRoof doesn't freeze while
// the callbook lookup runs. Failures pop up a message box.
//
// Optional LoggerInterface.ini next to the DLL:
//   [ACLog]
//   Host=127.0.0.1
//   Port=1100
//   LookupWaitMs=2000     ; time allowed for the callbook lookup
//   Debug=1               ; log the API conversation to Documents\SkyRoof_ACLog_debug.txt
//   Band2m=2              ; band names sent to AC Log if the rig-poll method
//   Band70cm=70CM         ;   does not set the band (fallback only)
//   Band23cm=23CM
//   Band13cm=13CM
//
// v4: QSOs are written to SkyRoof's own Adif folder
// (%APPDATA%\Afreet\Products\SkyRoof\Adif), and that folder is loaded at
// startup so Augment fills grid/state/name from earlier QSOs and GetStatus
// computes new grid / new state / dupe exactly like SkyRoof's built-in logger.
//   NewFileEvery=Year     ; Year, Month or Day - match SkyRoof's QSO Entry setting
//
// v3: band is set by simulating a rig poll on the uplink band (SENDRIGPOLL),
// so AC Log derives the band from the frequency itself. The result is read
// back with READBMF; CHANGEBM + field writes are used only as a fallback.
//
// Build (x64 Native Tools prompt, Visual Studio):
//   cl /LD /O2 /EHsc /MT LoggerInterface.cpp /link ws2_32.lib ole32.lib shell32.lib user32.lib
// Build (MinGW-w64):
//   x86_64-w64-mingw32-g++ -shared -O2 -static -o LoggerInterface.dll LoggerInterface.cpp -lws2_32 -lole32 -lshell32 -luser32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <string>
#include <map>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <set>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif

typedef std::map<std::string, std::string> Qso;

static HMODULE          g_module = nullptr;
static std::string      g_host = "127.0.0.1";
static std::string      g_port = "1100";
static int              g_lookupWaitMs = 2000;
static std::string      g_debugPath;
static std::string      g_adifDir;
static std::string      g_newFileEvery = "Year";
static bool             g_debug = true;
static std::string      g_band2m = "2", g_band70cm = "70CM", g_band23cm = "23CM", g_band13cm = "13CM";
static CRITICAL_SECTION g_logLock;   // one QSO at a time into AC Log

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH) g_module = hModule;
  return TRUE;
}

// ================================================================ helpers

static char* ReturnString(const std::string& s)
{
  char* p = (char*)CoTaskMemAlloc(s.size() + 1);
  if (p) memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

static void AppendUtf8(std::string& out, unsigned cp)
{
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// Reads a JSON string starting at the opening quote; p ends past the closing quote.
// System.Text.Json escapes characters like + < > & as \uXXXX, so those are decoded.
static std::string ReadJsonString(const char*& p)
{
  std::string out;
  if (*p != '"') return out;
  p++;
  while (*p && *p != '"')
  {
    if (*p == '\\' && p[1])
    {
      p++;
      switch (*p)
      {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': case 'f': break;
        case 'u':
        {
          unsigned cp = 0; int n = 0;
          while (n < 4 && p[n + 1])
          {
            char c = p[n + 1]; cp <<= 4;
            if (c >= '0' && c <= '9') cp |= c - '0';
            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
            n++;
          }
          AppendUtf8(out, cp);
          p += n;
          break;
        }
        default: out += *p; // \" \\ \/
      }
      p++;
    }
    else out += *p++;
  }
  if (*p == '"') p++;
  return out;
}

// Minimal parser for SkyRoof's flat QSO object.
static Qso ParseJson(const char* json)
{
  Qso m;
  if (!json) return m;
  const char* p = json;
  while (*p)
  {
    while (*p && *p != '"' && *p != '}') p++;
    if (*p != '"') break;
    std::string key = ReadJsonString(p);
    while (*p && *p != ':') p++;
    if (*p != ':') break;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    std::string value;
    if (*p == '"') value = ReadJsonString(p);
    else while (*p && *p != ',' && *p != '}') value += *p++;
    m[key] = value;
    while (*p && *p != ',' && *p != '}') p++;
    if (*p == ',') p++;
  }
  return m;
}

// Strip characters that would break ADIF or the API's tag format.
static std::string Clean(const std::string& value)
{
  std::string v;
  for (char c : value) if (c != '<' && c != '>' && c != '\r' && c != '\n') v += c;
  return v;
}

static std::string Upper(std::string s)
{
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

// SkyRoof band ("2m", "70cm", ...) -> AC Log band name (fallback path, set in .ini)
static std::string AcLogBand(const std::string& skyroofBand)
{
  std::string b = Upper(skyroofBand);
  if (b == "2M") return g_band2m;
  if (b == "70CM") return g_band70cm;
  if (b == "23CM") return g_band23cm;
  if (b == "13CM") return g_band13cm;
  return b;
}

// A frequency (MHz) inside the satellite segment of the band, for the simulated rig poll.
static std::string BandFrequency(const std::string& skyroofBand)
{
  std::string b = Upper(skyroofBand);
  if (b == "2M") return "145.900";
  if (b == "70CM") return "435.500";
  if (b == "23CM") return "1268.000";
  if (b == "13CM") return "2400.000";
  return "";
}

// Mode as a rig would report it, for the simulated rig poll.
static std::string RigMode(const std::string& acLogMode)
{
  if (acLogMode == "CW" || acLogMode == "FM" || acLogMode == "AM") return acLogMode;
  if (acLogMode == "SSB") return "USB";
  return "DIG";
}

static void DebugLog(const std::string& line)
{
  if (!g_debug || g_debugPath.empty()) return;
  FILE* f = fopen(g_debugPath.c_str(), "ab");
  if (!f) return;
  SYSTEMTIME st; GetSystemTime(&st);
  fprintf(f, "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line.c_str());
  fclose(f);
}

// SkyRoof "MFSK" (FT4) -> "FT4"; USB/LSB -> SSB
static std::string AcLogMode(const std::string& skyroofMode)
{
  std::string m = Upper(skyroofMode);
  if (m == "MFSK") return "FT4";
  if (m == "USB" || m == "LSB") return "SSB";
  return m;
}

// Parse SkyRoof's Utc ("2026-09-27T14:03:05.123Z"); false if malformed.
static bool ParseUtc(const std::string& utc, SYSTEMTIME& st)
{
  if (utc.size() < 19) return false;
  memset(&st, 0, sizeof(st));
  st.wYear   = (WORD)atoi(utc.substr(0, 4).c_str());
  st.wMonth  = (WORD)atoi(utc.substr(5, 2).c_str());
  st.wDay    = (WORD)atoi(utc.substr(8, 2).c_str());
  st.wHour   = (WORD)atoi(utc.substr(11, 2).c_str());
  st.wMinute = (WORD)atoi(utc.substr(14, 2).c_str());
  st.wSecond = (WORD)atoi(utc.substr(17, 2).c_str());
  return st.wYear > 2000 && st.wMonth >= 1 && st.wMonth <= 12;
}

// Minutes between the QSO time and now (UTC).
static double MinutesAgo(const SYSTEMTIME& qsoTime)
{
  FILETIME fq, fn;
  if (!SystemTimeToFileTime(&qsoTime, &fq)) return 0;
  GetSystemTimeAsFileTime(&fn);
  ULARGE_INTEGER a, b;
  a.LowPart = fq.dwLowDateTime; a.HighPart = fq.dwHighDateTime;
  b.LowPart = fn.dwLowDateTime; b.HighPart = fn.dwHighDateTime;
  return ((double)b.QuadPart - (double)a.QuadPart) / 600000000.0;
}

// ================================================================ backup ADIF

static void AddField(std::string& adif, const char* name, const std::string& value)
{
  std::string v = Clean(value);
  if (v.empty()) return;
  adif += "<"; adif += name; adif += ":" + std::to_string(v.size()) + ">" + v + " ";
}

static std::string BuildAdif(Qso& q)
{
  std::string date, time, utc = q["Utc"];
  if (utc.size() >= 19)
  {
    date = utc.substr(0, 4) + utc.substr(5, 2) + utc.substr(8, 2);
    time = utc.substr(11, 2) + utc.substr(14, 2) + utc.substr(17, 2);
  }
  std::string a;
  AddField(a, "STATION_CALLSIGN", q["StationCallsign"]);
  AddField(a, "MY_GRIDSQUARE", q["MyGridSquare"]);
  AddField(a, "QSO_DATE", date);
  AddField(a, "TIME_ON", time);
  AddField(a, "CALL", q["Call"]);
  AddField(a, "BAND", q["Band"]);
  AddField(a, "MODE", q["Mode"]);
  if (q["Mode"] == "MFSK") AddField(a, "SUBMODE", "FT4");
  AddField(a, "GRIDSQUARE", q["Grid"]);
  AddField(a, "STATE", q["State"]);
  AddField(a, "RST_SENT", q["Sent"]);
  AddField(a, "RST_RCVD", q["Recv"]);
  AddField(a, "NAME", q["Name"]);
  AddField(a, "COMMENT", q["Notes"]);
  if (!q["Sat"].empty())
  {
    AddField(a, "SAT_NAME", q["Sat"]);
    AddField(a, "PROP_MODE", "SAT");
  }
  a += "<EOR>";
  return a;
}

static std::string AdifPathFor(const std::string& utc)
{
  std::string y = utc.size() >= 10 ? utc.substr(0, 4) : "0000";
  std::string file;
  if (_stricmp(g_newFileEvery.c_str(), "Day") == 0 && utc.size() >= 10) file = utc.substr(0, 10);
  else if (_stricmp(g_newFileEvery.c_str(), "Month") == 0 && utc.size() >= 7) file = utc.substr(0, 7);
  else file = y;
  return g_adifDir + "\\" + file + ".adi";
}

static std::string g_lastAdifPath;

static void AppendToLog(const std::string& adif, const std::string& path)
{
  g_lastAdifPath = path;
  FILE* f = fopen(path.c_str(), "ab");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  if (ftell(f) == 0)
  {
    std::string hdr;
    AddField(hdr, "ADIF_VER", "3.1.4");
    AddField(hdr, "PROGRAMID", "SkyRoof-ACLog-bridge");
    hdr += "<EOH>\r\n";
    fputs(hdr.c_str(), f);
  }
  fputs(adif.c_str(), f);
  fputs("\r\n", f);
  fclose(f);
}

// ================================================================ worked-before lists
// Same logic as SkyRoof's AdifLogger.

static std::set<std::string> g_workedCalls, g_workedGrids, g_workedStates;
static std::map<std::string, std::string> g_gridLookup, g_stateLookup, g_nameLookup;

static const std::set<std::string> kUsStates = {
  "AL","AK","AZ","AR","CA","CO","CT","DE","FL","GA","HI","ID","IL","IN","IA","KS","KY","LA","ME","MD",
  "MA","MI","MN","MS","MO","MT","NE","NV","NH","NJ","NM","NY","NC","ND","OH","OK","OR","PA","RI","SC",
  "SD","TN","TX","UT","VT","VA","WA","WV","WI","WY" };

static bool IsValidState(const std::string& s) { return kUsStates.count(s) > 0; }

static bool IsValidGrid(const std::string& g)
{
  return g.size() == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' &&
         isdigit((unsigned char)g[2]) && isdigit((unsigned char)g[3]);
}

static void ImportRecord(Qso& r)
{
  std::string call = r["CALL"];
  if (call.empty()) return;
  bool isSat = Upper(r["PROP_MODE"]) == "SAT";

  std::string grid = Upper(r["GRIDSQUARE"]);
  grid = grid.size() >= 4 ? grid.substr(0, 4) : "";
  std::string state = Upper(r["STATE"]);
  if (!IsValidState(state)) state = "";

  if (isSat) g_workedCalls.insert(call);
  if (!grid.empty())  { g_gridLookup[call] = grid;  if (isSat) g_workedGrids.insert(grid); }
  if (!state.empty()) { g_stateLookup[call] = state; if (isSat) g_workedStates.insert(state); }
  if (!r["NAME"].empty()) g_nameLookup[call] = r["NAME"];
}

// Minimal ADIF reader: skips the header, splits records at <EOR>.
static void LoadAdifFile(const std::string& path)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return;
  std::string text;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);

  size_t p = 0;
  if (!text.empty() && text[0] != '<')        // a header exists unless the file starts with a tag
  {
    std::string up = Upper(text);
    size_t eoh = up.find("<EOH>");
    if (eoh != std::string::npos) p = eoh + 5;
  }

  Qso rec;
  while ((p = text.find('<', p)) != std::string::npos)
  {
    size_t close = text.find('>', p);
    if (close == std::string::npos) break;
    std::string tag = Upper(text.substr(p + 1, close - p - 1));
    p = close + 1;
    if (tag == "EOR") { ImportRecord(rec); rec.clear(); continue; }
    size_t c1 = tag.find(':');
    if (c1 == std::string::npos) continue;
    std::string name = tag.substr(0, c1);
    size_t len = (size_t)atoi(tag.c_str() + c1 + 1);
    if (p + len > text.size()) break;
    rec[name] = text.substr(p, len);
    p += len;
  }
}

static void BuildWorkedLists()
{
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((g_adifDir + "\\*.adi").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do { LoadAdifFile(g_adifDir + "\\" + fd.cFileName); } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// Add a just-saved QSO to the lists.
static void ImportSaved(Qso& q)
{
  Qso r;
  r["CALL"] = q["Call"];
  r["GRIDSQUARE"] = q["Grid"];
  r["STATE"] = q["State"];
  r["NAME"] = q["Name"];
  if (!q["Sat"].empty()) r["PROP_MODE"] = "SAT";
  ImportRecord(r);
}

// ================================================================ JSON writer

// UTF-8 -> JSON with non-ASCII as \uXXXX (the DLL boundary is ANSI, not UTF-8).
static std::string JsonEscape(const std::string& s)
{
  std::string o;
  for (size_t i = 0; i < s.size(); )
  {
    unsigned char c = (unsigned char)s[i];
    unsigned cp; int extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else { cp = '?'; extra = 0; }
    i++;
    for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);

    if (cp == '"') o += "\\\"";
    else if (cp == '\\') o += "\\\\";
    else if (cp < 0x20 || cp >= 0x80) { char u[8]; snprintf(u, sizeof(u), "\\u%04x", cp); o += u; }
    else o += (char)cp;
  }
  return o;
}

// Every QsoInfo member is a string (Utc as ISO text), so all values are written as strings.
static std::string ToJson(Qso& q)
{
  std::string o = "{";
  bool first = true;
  for (auto& kv : q)
  {
    if (!first) o += ",";
    first = false;
    o += "\"" + JsonEscape(kv.first) + "\":\"" + JsonEscape(kv.second) + "\"";
  }
  return o + "}";
}

// ================================================================ AC Log API

class AcLogConnection
{
public:
  ~AcLogConnection() { Close(); }

  std::string Open()
  {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(g_host.c_str(), g_port.c_str(), &hints, &res) != 0)
      return "Cannot resolve AC Log host " + g_host;
    for (addrinfo* a = res; a; a = a->ai_next)
    {
      s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s == INVALID_SOCKET) continue;
      if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
      closesocket(s);
      s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET)
      return "AC Log not reachable on " + g_host + ":" + g_port +
             " (is AC Log running with the TCP API enabled?)";
    return "";
  }

  bool Send(const std::string& cmd)
  {
    DebugLog(">> " + cmd);
    std::string line = cmd + "\r\n";
    size_t sent = 0;
    while (sent < line.size())
    {
      int n = send(s, line.c_str() + sent, (int)(line.size() - sent), 0);
      if (n <= 0) return false;
      sent += n;
    }
    Drain(25); // the API asks for at least 5 ms between commands
    return true;
  }

  bool Update(const char* control, const std::string& value)
  {
    return Send(std::string("<CMD><UPDATE><CONTROL>") + control +
                "</CONTROL><VALUE>" + Clean(value) + "</VALUE></CMD>");
  }

  bool Action(const char* action)
  {
    return Send(std::string("<CMD><ACTION><VALUE>") + action + "</VALUE></CMD>");
  }

  // Read whatever AC Log sends for up to ms milliseconds.
  void Drain(int ms)
  {
    DWORD end = GetTickCount() + ms;
    for (;;)
    {
      long left = (long)(end - GetTickCount());
      if (left <= 0) break;
      fd_set set; FD_ZERO(&set); FD_SET(s, &set);
      timeval tv{ left / 1000, (left % 1000) * 1000 };
      if (select(0, &set, nullptr, nullptr, &tv) <= 0) break;
      char buf[1024];
      int n = recv(s, buf, sizeof(buf), 0);
      if (n <= 0) break;
      rx.append(buf, n);
      DebugLog("<< " + std::string(buf, n));
    }
  }

  // Wait up to ms for a tag; returns the text that follows it.
  bool WaitFor(const std::string& tag, int ms, std::string& after)
  {
    DWORD end = GetTickCount() + ms;
    for (;;)
    {
      size_t pos = rx.find(tag);
      if (pos != std::string::npos) { after = rx.substr(pos + tag.size()); return true; }
      long left = (long)(end - GetTickCount());
      if (left <= 0) return false;
      Drain(left < 100 ? (int)left : 100);
    }
  }

  void ClearRx() { rx.clear(); }

  void Close()
  {
    if (s == INVALID_SOCKET) return;
    send(s, "\r\n", 2, 0); // a bare CR LF ends the API session
    closesocket(s);
    s = INVALID_SOCKET;
  }

private:
  SOCKET s = INVALID_SOCKET;
  std::string rx;
};

// Fill AC Log's entry form and press Enter. Returns "" on success.
static std::string LogToAcLog(Qso& q)
{
  AcLogConnection c;
  std::string err = c.Open();
  if (!err.empty()) return err;

  // 1. call sign + CALLTAB = AC Log's callbook lookup and previous-QSO fill
  if (!c.Action("CLEAR")) return "Lost connection to AC Log";
  c.Update("TXTENTRYCALL", Upper(q["Call"]));
  c.Action("CALLTAB");
  c.Drain(g_lookupWaitMs);

  // 2. band and mode
  std::string band = AcLogBand(q["Band"]);
  std::string mode = AcLogMode(q["Mode"]);
  std::string freq = BandFrequency(q["Band"]);
  bool bandOk = false;

  // 2a. simulate a rig poll on the uplink band; AC Log derives the band itself
  if (!freq.empty())
  {
    c.Send("<CMD><SENDRIGPOLL><FREQ>" + freq + "</FREQ><MODE>" + RigMode(mode) + "</MODE></CMD>");
    c.Send("<CMD><IGNORERIGPOLLS><VALUE>TRUE</VALUE></CMD>");
    c.Drain(200);
    c.ClearRx();
    c.Send("<CMD><READBMF></CMD>");
    std::string after;
    if (c.WaitFor("<READBMFRESPONSE><BAND>", 1500, after))
    {
      std::string got = Upper(after.substr(0, after.find('<')));
      std::string want = Upper(q["Band"]);
      std::string wantNum = want.substr(0, want.find_first_not_of("0123456789."));
      bandOk = !got.empty() && (got == band || got == want || got.compare(0, wantNum.size(), wantNum) == 0) &&
               got != "6" && got.compare(0, 2, "6M") != 0;  // 6 m is never a satellite band
      DebugLog("READBMF band '" + got + "', wanted '" + q["Band"] + "' -> " + (bandOk ? "OK" : "fallback"));
    }
  }
  else
    c.Send("<CMD><IGNORERIGPOLLS><VALUE>TRUE</VALUE></CMD>");

  // 2b. mode (and band only if the rig poll did not set it)
  c.Send("<CMD><CHANGEBM><BAND>" + std::string(bandOk ? "" : band) + "</BAND><MODE>" + mode + "</MODE></CMD>");
  if (!bandOk)
  {
    c.Update("TXTENTRYBAND", band);
    c.Update("TXTENTRYMODE", mode);
  }
  // the simulated poll frequency is not the real one, so don't log it
  c.Update("TXTENTRYFREQUENCY", "");

  // 3. date/time only for older QSOs; for fresh ones AC Log's clock is right
  SYSTEMTIME st;
  if (ParseUtc(q["Utc"], st) && MinutesAgo(st) > 2.0)
  {
    char date[16], time[8];
    snprintf(date, sizeof(date), "%04u/%02u/%02u", st.wYear, st.wMonth, st.wDay);
    snprintf(time, sizeof(time), "%02u:%02u", st.wHour, st.wMinute);
    c.Update("TXTENTRYDATE", date);
    c.Update("TXTENTRYTIMEON", time);
    c.Update("TXTENTRYTIMEOFF", time);
  }

  // 4. what SkyRoof knows; only non-empty values, so lookup data is kept
  if (!q["Sent"].empty())  c.Update("TXTENTRYRSTS", q["Sent"]);
  if (!q["Recv"].empty())  c.Update("TXTENTRYRSTR", q["Recv"]);
  if (!q["Grid"].empty())  c.Update("TXTENTRYGRID", q["Grid"]);
  if (!q["Name"].empty())  c.Update("TXTENTRYNAMER", q["Name"]);
  if (!q["State"].empty()) c.Update("TXTENTRYSTATE", q["State"]);
  if (!q["Notes"].empty()) c.Update("TXTENTRYCOMMENTS", q["Notes"]);
  if (!q["Sat"].empty())
  {
    c.Update("TXTENTRYSATNAME", q["Sat"]);
    c.Update("TXTENTRYPROPMODE", "SAT");
  }

  // 5. log it
  c.Action("ENTER");
  std::string after;
  bool gotResponse = c.WaitFor("<ENTERRESPONSE><VALUE>", 3000, after);
  c.Send("<CMD><IGNORERIGPOLLS><VALUE>FALSE</VALUE></CMD>");
  c.Close();

  if (gotResponse && !after.empty() && after[0] == '0')
    return "AC Log did not add the QSO with " + q["Call"] + " (check AC Log for a message)";
  return "";
}

// ================================================================ worker thread

static DWORD WINAPI LogThread(LPVOID param)
{
  Qso* q = (Qso*)param;
  EnterCriticalSection(&g_logLock);
  std::string err = LogToAcLog(*q);
  LeaveCriticalSection(&g_logLock);

  if (!err.empty())
  {
    std::string msg = err + "\n\nThe QSO is saved in SkyRoof's log\n" + g_lastAdifPath +
                      "\nImport it into AC Log later.";
    MessageBoxA(nullptr, msg.c_str(), "SkyRoof to AC Log",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
  }
  delete q;
  return 0;
}

// ================================================================ exports

extern "C" {

__declspec(dllexport) void __cdecl Init()
{
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  InitializeCriticalSection(&g_logLock);

  // optional settings file next to the DLL
  char path[MAX_PATH] = {0};
  GetModuleFileNameA(g_module, path, MAX_PATH);
  std::string ini = path;
  size_t slash = ini.find_last_of("\\/");
  ini = (slash == std::string::npos ? std::string() : ini.substr(0, slash + 1)) + "LoggerInterface.ini";
  char val[256];
  GetPrivateProfileStringA("ACLog", "Host", g_host.c_str(), val, sizeof(val), ini.c_str());
  g_host = val;
  GetPrivateProfileStringA("ACLog", "Port", g_port.c_str(), val, sizeof(val), ini.c_str());
  g_port = val;
  g_lookupWaitMs = (int)GetPrivateProfileIntA("ACLog", "LookupWaitMs", g_lookupWaitMs, ini.c_str());
  g_debug = GetPrivateProfileIntA("ACLog", "Debug", 1, ini.c_str()) != 0;
  GetPrivateProfileStringA("ACLog", "Band2m", g_band2m.c_str(), val, sizeof(val), ini.c_str());     g_band2m = val;
  GetPrivateProfileStringA("ACLog", "Band70cm", g_band70cm.c_str(), val, sizeof(val), ini.c_str()); g_band70cm = val;
  GetPrivateProfileStringA("ACLog", "Band23cm", g_band23cm.c_str(), val, sizeof(val), ini.c_str()); g_band23cm = val;
  GetPrivateProfileStringA("ACLog", "Band13cm", g_band13cm.c_str(), val, sizeof(val), ini.c_str()); g_band13cm = val;
  GetPrivateProfileStringA("ACLog", "NewFileEvery", g_newFileEvery.c_str(), val, sizeof(val), ini.c_str()); g_newFileEvery = val;

  // SkyRoof's own ADIF folder: %APPDATA%\Afreet\Products\SkyRoof\Adif
  char appData[MAX_PATH] = {0};
  if (SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData) == S_OK)
  {
    g_adifDir = std::string(appData) + "\\Afreet\\Products\\SkyRoof\\Adif";
    SHCreateDirectoryExA(nullptr, g_adifDir.c_str(), nullptr);
  }

  // debug log in Documents
  char docs[MAX_PATH] = {0};
  if (SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs) == S_OK)
    g_debugPath = std::string(docs) + "\\SkyRoof_ACLog_debug.txt";

  BuildWorkedLists();
  DebugLog("Init: Adif folder " + g_adifDir + ", " + std::to_string(g_workedCalls.size()) + " satellite calls worked");
}

__declspec(dllexport) char* __cdecl SaveQso(const char* json)
{
  DebugLog(std::string("SaveQso ") + (json ? json : ""));
  Qso* q = new Qso(ParseJson(json));
  if ((*q)["Call"].empty()) { delete q; return ReturnString("No callsign in QSO"); }

  // SkyRoof's ADIF log first, so nothing is lost whatever happens next
  AppendToLog(BuildAdif(*q), AdifPathFor((*q)["Utc"]));
  ImportSaved(*q);

  // AC Log entry runs in the background (the lookup takes a second or two)
  HANDLE h = CreateThread(nullptr, 0, LogThread, q, 0, nullptr);
  if (!h) { delete q; return ReturnString("Could not start the AC Log thread; QSO saved to " + g_lastAdifPath); }
  CloseHandle(h);
  return ReturnString("");
}

// Fill grid, state and name from earlier QSOs (same as SkyRoof's AdifLogger).
__declspec(dllexport) char* __cdecl Augment(const char* json)
{
  Qso q = ParseJson(json);
  std::string call = q["Call"];
  if (call.empty()) return ReturnString(json ? json : "");
  if (q["Grid"].empty()  && g_gridLookup.count(call))  q["Grid"]  = g_gridLookup[call];
  if (q["State"].empty() && g_stateLookup.count(call)) q["State"] = g_stateLookup[call];
  if (q["Name"].empty()  && g_nameLookup.count(call))  q["Name"]  = g_nameLookup[call];
  return ReturnString(ToJson(q));
}

// New grid / new state / dupe status (same as SkyRoof's AdifLogger). SkyRoof
// currently declares StatusString/BackColor/ForeColor as fields, which its JSON
// serializer skips, so these values take effect only once SkyRoof includes them.
__declspec(dllexport) char* __cdecl GetStatus(const char* json)
{
  Qso q = ParseJson(json);
  std::string grid = Upper(q["Grid"]), state = Upper(q["State"]);
  bool dupe = g_workedCalls.count(q["Call"]) > 0;
  bool newState = IsValidState(state) && !g_workedStates.count(state);
  bool newGrid = IsValidGrid(grid) && !g_workedGrids.count(grid);

  if (newGrid) q["BackColor"] = "#00FF00";
  else if (newState) q["BackColor"] = "#88FF88";
  else if (dupe) q["BackColor"] = "#CCCCCC";

  if (newState && newGrid) q["StatusString"] = "New state and grid";
  else if (newState) q["StatusString"] = "New state";
  else if (newGrid) q["StatusString"] = "New grid";
  else if (dupe) q["StatusString"] = "Duplicate";
  else if (!newGrid && !IsValidState(state)) q["StatusString"] = "Status unknown";
  else if (!newState && !IsValidGrid(grid)) q["StatusString"] = "Status unknown";
  else q["StatusString"] = "Not needed";

  return ReturnString(ToJson(q));
}

} // extern "C"
