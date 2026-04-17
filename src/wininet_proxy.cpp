#include <windows.h>
#include <wininet.h>

#include "compat.h"

static HMODULE hRealWininet = NULL;

// Real function pointers
static decltype(&InternetOpenA) pInternetOpenA = nullptr;
static decltype(&InternetOpenUrlA) pInternetOpenUrlA = nullptr;
static decltype(&InternetSetCookieA) pInternetSetCookieA = nullptr;
static decltype(&InternetCloseHandle) pInternetCloseHandle = nullptr;
static decltype(&InternetReadFile) pInternetReadFile = nullptr;

void LoadRealWininet() {
  char path[MAX_PATH];
  GetSystemDirectoryA(path, MAX_PATH);
  strcat_s(path, "\\wininet.dll");
  hRealWininet = LoadLibraryA(path);
  if (!hRealWininet)
    return;

  pInternetOpenA =
      (decltype(&InternetOpenA))GetProcAddress(hRealWininet, "InternetOpenA");
  pInternetOpenUrlA = (decltype(&InternetOpenUrlA))GetProcAddress(
      hRealWininet, "InternetOpenUrlA"
  );
  pInternetSetCookieA = (decltype(&InternetSetCookieA))GetProcAddress(
      hRealWininet, "InternetSetCookieA"
  );
  pInternetCloseHandle = (decltype(&InternetCloseHandle))GetProcAddress(
      hRealWininet, "InternetCloseHandle"
  );
  pInternetReadFile = (decltype(&InternetReadFile))GetProcAddress(
      hRealWininet, "InternetReadFile"
  );
}

void UnloadRealWininet() {
  if (hRealWininet) {
    FreeLibrary(hRealWininet);
    hRealWininet = NULL;
  }
}

// Exported forwarding functions
extern "C" {

HINTERNET WINAPI ProxyInternetOpenA(
    LPCSTR lpszAgent,
    DWORD dwAccessType,
    LPCSTR lpszProxy,
    LPCSTR lpszProxyBypass,
    DWORD dwFlags
) {
  if (pInternetOpenA)
    return pInternetOpenA(
        lpszAgent, dwAccessType, lpszProxy, lpszProxyBypass, dwFlags
    );
  return NULL;
}

HINTERNET WINAPI ProxyInternetOpenUrlA(
    HINTERNET hInternet,
    LPCSTR lpszUrl,
    LPCSTR lpszHeaders,
    DWORD dwHeadersLength,
    DWORD dwFlags,
    DWORD_PTR dwContext
) {
  if (pInternetOpenUrlA)
    return pInternetOpenUrlA(
        hInternet, lpszUrl, lpszHeaders, dwHeadersLength, dwFlags, dwContext
    );
  return NULL;
}

BOOL WINAPI ProxyInternetSetCookieA(
    LPCSTR lpszUrl, LPCSTR lpszCookieName, LPCSTR lpszCookieData
) {
  if (pInternetSetCookieA)
    return pInternetSetCookieA(lpszUrl, lpszCookieName, lpszCookieData);
  return FALSE;
}

BOOL WINAPI ProxyInternetCloseHandle(HINTERNET hInternet) {
  if (pInternetCloseHandle)
    return pInternetCloseHandle(hInternet);
  return FALSE;
}

BOOL WINAPI ProxyInternetReadFile(
    HINTERNET hFile,
    LPVOID lpBuffer,
    DWORD dwNumberOfBytesToRead,
    LPDWORD lpdwNumberOfBytesRead
) {
  if (pInternetReadFile)
    return pInternetReadFile(
        hFile, lpBuffer, dwNumberOfBytesToRead, lpdwNumberOfBytesRead
    );
  return FALSE;
}

} // extern "C"
