#ifndef __WINE_WINVER_H
#define __WINE_WINVER_H

DWORD       WINAPI VerFindFile16(UINT16,LPCSTR,LPCSTR,LPCSTR,LPSTR,UINT16*,LPSTR,UINT16*);
DWORD       WINAPI VerFindFile32A(UINT32,LPCSTR,LPCSTR,LPCSTR,LPSTR,UINT32*,LPSTR,UINT32*);
DWORD       WINAPI VerFindFile32W(UINT32,LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,UINT32*,LPWSTR,UINT32*);
#define     VerFindFile WINELIB_NAME_AW(VerFindFile)
DWORD       WINAPI VerInstallFile16(UINT16,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPSTR,UINT16*);
DWORD       WINAPI VerInstallFile32A(UINT32,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPSTR,UINT32*);
DWORD       WINAPI VerInstallFile32W(UINT32,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,UINT32*);
#define     VerInstallFile WINELIB_NAME_AW(VerInstallFile)
DWORD       WINAPI VerLanguageName16(UINT16,LPSTR,UINT16);
DWORD       WINAPI VerQueryValue16(SEGPTR,LPCSTR,SEGPTR*,UINT16*);
DWORD       WINAPI VerQueryValue32A(LPVOID,LPCSTR,LPVOID*,UINT32*);
DWORD       WINAPI VerQueryValue32W(LPVOID,LPCWSTR,LPVOID*,UINT32*);
#define     VerQueryValue WINELIB_NAME_AW(VerQueryValue)
DWORD       WINAPI GetFileVersionInfoSize16(LPCSTR,LPDWORD);
DWORD       WINAPI GetFileVersionInfoSize32A(LPCSTR,LPDWORD);
DWORD       WINAPI GetFileVersionInfoSize32W(LPCWSTR,LPDWORD);
#define     GetFileVersionInfoSize WINELIB_NAME_AW(GetFileVersionInfoSize)
DWORD       WINAPI GetFileVersionInfo16(LPCSTR,DWORD,DWORD,LPVOID);
DWORD       WINAPI GetFileVersionInfo32A(LPCSTR,DWORD,DWORD,LPVOID);
DWORD       WINAPI GetFileVersionInfo32W(LPCWSTR,DWORD,DWORD,LPVOID);
#define     GetFileVersionInfo WINELIB_NAME_AW(GetFileVersionInfo)

#endif /* __WINE_WINVER_H */
