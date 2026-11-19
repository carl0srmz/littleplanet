#include "app.h"
#ifdef _WIN32
#include <winhttp.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

extern const char build_id[];

// Updates follow the latest release (whatever its version): its tag is moved to each build the release
// gets, so a copy is up to date when its build is the commit that tag names.
#define OWNER_REPO "carl0srmz/littleplanet"
#define REPO "https://github.com/" OWNER_REPO
#define API_PATH "/repos/" OWNER_REPO

static volatile int state;
static char tmp[1024];
static char tag[32];   // the latest release's tag (checked to be [0-9A-Za-z._-]: it goes into URLs and a shell command)

#ifdef _WIN32
// Windows: the check reads GitHub's answer in memory, and "Update" opens the release page in the browser.
// (A game that downloads an .exe and runs it is what antivirus heuristics call a trojan downloader.)
static bool get(const wchar_t *host, const wchar_t *path, char *out, int max) {
  bool ok = false; int n = 0;
  HINTERNET s = WinHttpOpen(L"LittlePlanet/" LP_VERSION_WNUM, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0), c = NULL, r = NULL;
  if (s) c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
  if (r && WinHttpSendRequest(r, L"User-Agent: LittlePlanet\r\n", (DWORD)-1, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
    DWORD got;
    while (n < max - 1 && WinHttpReadData(r, out + n, max - 1 - n, &got) && got) n += got;
    ok = n > 0;
  }
  out[n] = 0;
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  return ok;
}
#else
#ifdef __APPLE__
#define ASSET "littleplanet-macos.dmg"
#elif defined(__aarch64__)
#define ASSET "littleplanet-linux-aarch64.AppImage"
#else
#define ASSET "littleplanet-linux.AppImage"
#endif
static bool fetch(const char *url, const char *path) {
  char c[2048]; snprintf(c, sizeof c, "curl -fsSL --connect-timeout 10 --max-time 300 -o '%s' '%s' </dev/null >/dev/null 2>&1", path, url);
  return system(c) == 0;
}
static void tmp_path(const char *name) {
  const char *a = getenv("APPIMAGE");
  if (a && !strcmp(name, ASSET)) snprintf(tmp, sizeof tmp, "%s.new", a);
  else snprintf(tmp, sizeof tmp, "/tmp/%s", name);
}
#endif

// GitHub's API, path from api.github.com, into buf
static void api_get(const char *path, char *buf, int max) {
  buf[0] = 0;
#ifdef _WIN32
  wchar_t wp[256];
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 256);
  get(L"api.github.com", wp, buf, max);
#else
  char url[512]; snprintf(url, sizeof url, "https://api.github.com%s", path);
  tmp_path("littleplanet-api.json");
  FILE *f = fetch(url, tmp) ? fopen(tmp, "rb") : NULL;
  if (f) { buf[fread(buf, 1, max - 1, f)] = 0; fclose(f); remove(tmp); }
#endif
}

// the string value of "key" in a JSON text, if it's a plain one (no escapes) that fits
static bool json_str(const char *buf, const char *key, char *out, int max) {
  char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
  const char *p = strstr(buf, k);
  if (!p) return false;
  p += strlen(k);
  while (*p == ' ' || *p == ':') p++;
  if (*p++ != '"') return false;
  int n = 0;
  while (p[n] && p[n] != '"') { if (p[n] == '\\' || n >= max - 1) return false; n++; }
  if (p[n] != '"') return false;
  memcpy(out, p, n); out[n] = 0;
  return true;
}

static int check(void *arg) {
  (void)arg;
  static char buf[16384];   // (the release's JSON names its tag after its author; the assets come later)
  char t[32], sha[64];
  api_get(API_PATH "/releases/latest", buf, sizeof buf);
  if (!json_str(buf, "tag_name", t, sizeof t) || !t[0]) { state = 0; return 0; }
  for (const char *c = t; *c; c++)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '_' || *c == '-')) { state = 0; return 0; }
  char path[128]; snprintf(path, sizeof path, API_PATH "/git/ref/tags/%s", t);
  api_get(path, buf, sizeof buf);
  if (!json_str(buf, "sha", sha, sizeof sha) || strlen(sha) != 40) { state = 0; return 0; }
  snprintf(tag, sizeof tag, "%s", t);
  state = strncmp(sha, build_id, 40) ? 3 : 2;
  return 0;
}

#ifndef _WIN32
static int download(void *arg) {
  (void)arg;
  char url[256]; snprintf(url, sizeof url, REPO "/releases/download/%s/" ASSET, tag);
  tmp_path(ASSET);
  state = fetch(url, tmp) ? 5 : 6;
  return 0;
}
#endif

void update_start_check(void) {
  extern const char *uitest_prefix;
  if (strlen(build_id) == 40 && !state && !uitest_prefix) { state = 1; plat_thread(check); }
}

const char *update_label(void) {
  static const char *l[] = { NULL, NULL, "Up to date", "Update", "Downloading...", "Downloading...", "Update failed", "Restarting..." };
  return l[state];
}

bool update_ready(void) { return state == 3; }
#ifdef _WIN32
void update_download(void) {
  if (state != 3) return;
  char url[256]; snprintf(url, sizeof url, REPO "/releases/tag/%s", tag);
  ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}
#else
void update_download(void) { if (state == 3) { state = 4; plat_thread(download); } }
#endif

bool update_apply(void) {
  if (state != 5) return false;
#ifdef _WIN32
#elif defined(__APPLE__)
  char c[1200]; snprintf(c, sizeof c, "open '%s'", tmp);
  system(c);
#else
  const char *a = getenv("APPIMAGE");
  if (a) {
    chmod(tmp, 0755);
    if (rename(tmp, a) == 0) {
      char c[1200]; snprintf(c, sizeof c, "'%s' >/dev/null 2>&1 &", a);
      system(c);
    } else { state = 6; return false; }
  } else { char c[300]; snprintf(c, sizeof c, "xdg-open '" REPO "/releases/tag/%s' >/dev/null 2>&1 &", tag); system(c); }
#endif
  state = 7;
  return true;
}
