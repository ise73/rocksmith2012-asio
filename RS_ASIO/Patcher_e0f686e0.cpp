#include "stdafx.h"
#include "dllmain.h"
#include "Patcher.h"
#include <setupapi.h>
#include <mmsystem.h>
#include <dbt.h>
#include <sstream>
#include <map>

// ---------------------------------------------------------------------------
// SetupAPI and CreateFile diagnostic hooks (pass-through, logging only).
// These let us see exactly what the game does after our registry entries are
// created: whether SetupAPI finds them, what device path it gets, and whether
// the game then tries CreateFile on that path.
// ---------------------------------------------------------------------------

// IAT RVAs for the diagnostic functions (objdump format; actual address = ImageBase + RVA)
static const DWORD IAT_RVA_SetupDiGetClassDevsA          = 0x0088d2d4;
static const DWORD IAT_RVA_SetupDiEnumDeviceInterfaces   = 0x0088d2d8;
static const DWORD IAT_RVA_SetupDiGetDeviceInterfaceDetailW = 0x0088d2c8;
static const DWORD IAT_RVA_SetupDiGetDeviceInterfaceAlias  = 0x0088d2d0;
static const DWORD IAT_RVA_SetupDiOpenDeviceInterfaceRegKey = 0x0088d2c0;
static const DWORD IAT_RVA_SetupDiDestroyDeviceInfoList    = 0x0088d2cc;
static const DWORD IAT_RVA_CreateFileW                     = 0x0088d128;

typedef HDEVINFO (WINAPI* PFN_SetupDiGetClassDevsA)(const GUID*, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI* PFN_SetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI* PFN_SetupDiGetDeviceInterfaceDetailW)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI* PFN_SetupDiGetDeviceInterfaceAlias)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, const GUID*, PSP_DEVICE_INTERFACE_DATA);
typedef HKEY (WINAPI* PFN_SetupDiOpenDeviceInterfaceRegKey)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, DWORD, REGSAM);
typedef BOOL (WINAPI* PFN_SetupDiDestroyDeviceInfoList)(HDEVINFO);
typedef HANDLE (WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static PFN_SetupDiGetClassDevsA           s_Real_SetupDiGetClassDevsA          = nullptr;
static PFN_SetupDiEnumDeviceInterfaces    s_Real_SetupDiEnumDeviceInterfaces   = nullptr;
static PFN_SetupDiGetDeviceInterfaceDetailW s_Real_SetupDiGetDeviceInterfaceDetailW = nullptr;
static PFN_SetupDiGetDeviceInterfaceAlias  s_Real_SetupDiGetDeviceInterfaceAlias = nullptr;
static PFN_SetupDiOpenDeviceInterfaceRegKey s_Real_SetupDiOpenDeviceInterfaceRegKey = nullptr;
static PFN_SetupDiDestroyDeviceInfoList    s_Real_SetupDiDestroyDeviceInfoList  = nullptr;
static PFN_CreateFileW                     s_Real_CreateFileW                   = nullptr;

static std::string DiagFmtGuid(const GUID* g)
{
	if (!g) return "(null)";
	char buf[40];
	sprintf_s(buf, sizeof(buf),
	          "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
	          g->Data1, g->Data2, g->Data3,
	          g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
	          g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
	return std::string(buf);
}

// Returns a human-readable string for common Windows error codes so log readers
// do not need to look up numeric codes manually.
static std::string WinErrStr(DWORD gle)
{
	switch (gle)
	{
	case ERROR_SUCCESS:             return "0 (ERROR_SUCCESS)";
	case ERROR_FILE_NOT_FOUND:      return "2 (ERROR_FILE_NOT_FOUND - no matching registry/device entries)";
	case ERROR_ACCESS_DENIED:       return "5 (ERROR_ACCESS_DENIED)";
	case ERROR_INVALID_HANDLE:      return "6 (ERROR_INVALID_HANDLE)";
	case ERROR_INVALID_PARAMETER:   return "87 (ERROR_INVALID_PARAMETER)";
	case ERROR_INSUFFICIENT_BUFFER: return "122 (ERROR_INSUFFICIENT_BUFFER)";
	case ERROR_NO_MORE_ITEMS:       return "259 (ERROR_NO_MORE_ITEMS - end of enumeration, normal)";
	default:
		return std::to_string(gle);
	}
}

static HDEVINFO WINAPI Diag_SetupDiGetClassDevsA(const GUID* ClassGuid, PCSTR Enumerator, HWND hwndParent, DWORD Flags)
{
	HDEVINFO ret = s_Real_SetupDiGetClassDevsA(ClassGuid, Enumerator, hwndParent, Flags);
	DWORD gle = GetLastError();
	rslog::info_ts() << "Patched_SetupDiGetClassDevsA - flags=0x" << std::hex << Flags
	                 << " classGuid=" << DiagFmtGuid(ClassGuid)
	                 << " -> " << ret << " gle=" << WinErrStr(gle);
	if (ret == INVALID_HANDLE_VALUE)
		rslog::info_ts() << " [WARN: no device info set returned - KS registry entries for the cable may be"
		                    " missing, malformed, or Control\\Linked is not set; cable scan cannot proceed]";
	else
		rslog::info_ts() << " [OK: device info set created - will enumerate interfaces below]";
	rslog::info_ts() << std::endl;
	return ret;
}

static BOOL WINAPI Diag_SetupDiEnumDeviceInterfaces(HDEVINFO Set, PSP_DEVINFO_DATA DevData, const GUID* IfaceGuid, DWORD MemberIdx, PSP_DEVICE_INTERFACE_DATA IfaceData)
{
	BOOL ret = s_Real_SetupDiEnumDeviceInterfaces(Set, DevData, IfaceGuid, MemberIdx, IfaceData);
	DWORD gle = GetLastError();
	rslog::info_ts() << "Patched_SetupDiEnumDeviceInterfaces - MemberIndex=" << MemberIdx
	                 << " ifaceClassGuid=" << DiagFmtGuid(IfaceGuid)
	                 << " -> " << ret << " gle=" << WinErrStr(gle);
	if (ret)
		rslog::info_ts() << " [interface found - alias queries and detail fetch will follow]";
	else if (gle == ERROR_NO_MORE_ITEMS)
		rslog::info_ts() << " [end of interface list - normal termination of scan]";
	else if (MemberIdx == 0)
		rslog::info_ts() << " [WARN: first interface (index 0) not found - no cable KS interfaces are"
		                    " registered; check EnsureRealToneCableRegistered output above]";
	else
		rslog::info_ts() << " [no interface at this index]";
	rslog::info_ts() << std::endl;
	return ret;
}

static BOOL WINAPI Diag_SetupDiGetDeviceInterfaceDetailW(HDEVINFO Set, PSP_DEVICE_INTERFACE_DATA IfaceData, PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail, DWORD DetailSize, PDWORD RequiredSize, PSP_DEVINFO_DATA DevData)
{
	BOOL ret = s_Real_SetupDiGetDeviceInterfaceDetailW(Set, IfaceData, Detail, DetailSize, RequiredSize, DevData);
	DWORD gle = GetLastError();
	rslog::info_ts() << "Patched_SetupDiGetDeviceInterfaceDetailW - detailSize=" << DetailSize
	                 << " -> " << ret << " gle=" << WinErrStr(gle);
	if (ret && Detail && Detail->DevicePath[0])
	{
		rslog::info_ts() << " path: " << std::wstring(Detail->DevicePath);
		// Annotate the path type so it's easy to tell which entry the game is processing
		std::wstring lpath(Detail->DevicePath);
		std::transform(lpath.begin(), lpath.end(), lpath.begin(), [](wchar_t c){ return towlower(c); });
		if (lpath.find(L"rs_asio") != std::wstring::npos)
			rslog::info_ts() << " [synthetic RS_ASIO entry - no real KS device; CreateFileW will be redirected to NUL]";
		else if (lpath.find(L"vid_12ba") != std::wstring::npos)
			rslog::info_ts() << " [real cable hardware path - Wine KS open will likely fail under winepipewire]";
	}
	else if (!ret)
		rslog::info_ts() << " [WARN: could not retrieve device path]";
	rslog::info_ts() << std::endl;
	return ret;
}

static BOOL WINAPI Diag_SetupDiGetDeviceInterfaceAlias(HDEVINFO Set, PSP_DEVICE_INTERFACE_DATA IfaceData, const GUID* AliasGuid, PSP_DEVICE_INTERFACE_DATA AliasIfaceData)
{
	// SetupDiGetDeviceInterfaceAlias is "@ stub" (aborting) in this Wine build.
	// The game queries aliases for KSCATEGORY_AUDIO_DEVICE and KSCATEGORY_AUDIO_CONTROL
	// BEFORE calling SetupDiGetDeviceInterfaceDetailW.  If either alias returns FALSE,
	// the game skips the device entirely (CreateFileW is never reached).
	//
	// Synthesize a successful alias by reusing the source interface's Reserved pointer.
	// Wine's SetupDiGetDeviceInterfaceDetailW (and get_iface()) uses only Reserved to
	// locate the internal device_iface — it does NOT validate the GUID or check set
	// membership — so the game will receive our fake SymbolicLink path for any alias GUID.
	rslog::info_ts() << "Patched_SetupDiGetDeviceInterfaceAlias - aliasClassGuid=" << DiagFmtGuid(AliasGuid);

	if (!IfaceData || !IfaceData->Reserved)
	{
		rslog::info_ts() << " -> FALSE (invalid source IfaceData)" << std::endl;
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (AliasIfaceData)
	{
		// cbSize is set by the caller per API convention; fill in the remaining fields.
		AliasIfaceData->InterfaceClassGuid = *AliasGuid;
		AliasIfaceData->Flags  = SPINT_ACTIVE;           // interface is present / active
		AliasIfaceData->Reserved = IfaceData->Reserved;  // same Wine-internal device_iface ptr
	}
	rslog::info_ts() << " -> TRUE (alias synthesized from source Reserved)" << std::endl;
	return TRUE;
}

static HKEY WINAPI Diag_SetupDiOpenDeviceInterfaceRegKey(HDEVINFO Set, PSP_DEVICE_INTERFACE_DATA IfaceData, DWORD Reserved, REGSAM samDesired)
{
	HKEY ret = s_Real_SetupDiOpenDeviceInterfaceRegKey(Set, IfaceData, Reserved, samDesired);
	DWORD gle = GetLastError();
	rslog::info_ts() << "Patched_SetupDiOpenDeviceInterfaceRegKey - samDesired=0x" << std::hex << samDesired
	                 << " -> " << ret << " gle=" << WinErrStr(gle);
	if (ret == INVALID_HANDLE_VALUE)
		rslog::info_ts() << " [WARN: registry key open failed - the interface's DeviceClasses key may be"
		                    " incomplete; the game may skip this interface]";
	else
		rslog::info_ts() << " [OK: registry key opened]";
	rslog::info_ts() << std::endl;
	return ret;
}

static BOOL WINAPI Diag_SetupDiDestroyDeviceInfoList(HDEVINFO Set)
{
	BOOL ret = s_Real_SetupDiDestroyDeviceInfoList(Set);
	rslog::info_ts() << "Patched_SetupDiDestroyDeviceInfoList -> " << ret << " gle=" << WinErrStr(GetLastError())
	                 << " [KS cable scan complete - see CreateFileW results above for outcome]" << std::endl;
	return ret;
}

static bool IsCableDevicePath(LPCWSTR path)
{
	if (!path) return false;
	// Case-insensitive search: our SymbolicLink is stored lowercase.
	wchar_t lower[512];
	DWORD len = 0;
	while (path[len] && len < (ARRAYSIZE(lower) - 1)) { lower[len] = (wchar_t)towlower(path[len]); ++len; }
	lower[len] = L'\0';
	return wcsstr(lower, L"vid_12ba") != nullptr || wcsstr(lower, L"rs_asio") != nullptr;
}

static bool IsFakeRsAsioCablePath(LPCWSTR path)
{
	// Returns true ONLY for our synthetic RS_ASIO entry.
	// Real cable paths registered by winepipewire (e.g. vid_12ba...7182&2ad191bf) must NOT
	// be intercepted — Wine's KS subsystem can open them directly, the same way it handles
	// the winepulse-registered path when PROTON_USE_PIPEWIRE=0.
	if (!path) return false;
	wchar_t lower[512];
	DWORD len = 0;
	while (path[len] && len < (ARRAYSIZE(lower) - 1)) { lower[len] = (wchar_t)towlower(path[len]); ++len; }
	lower[len] = L'\0';
	return wcsstr(lower, L"rs_asio") != nullptr;
}

// ---------------------------------------------------------------------------
// Extra diagnostics (cable-free investigation): what does the game do with the
// KS device handles it opens, which SetupAPI registry properties it reads, and
// whether it touches the legacy waveIn API.
// ---------------------------------------------------------------------------

static const DWORD IAT_RVA_DeviceIoControl                    = 0x0088d070;
static const DWORD IAT_RVA_SetupDiGetClassDevsW               = 0x0088d2bc;
static const DWORD IAT_RVA_SetupDiGetDeviceRegistryPropertyW  = 0x0088d2c4;
static const DWORD IAT_RVA_RegisterDeviceNotificationW        = 0x0088d3b0;
static const DWORD IAT_RVA_waveInOpen                         = 0x0088d3e4;
static const DWORD IAT_RVA_waveInGetNumDevs                   = 0x0088d3fc;
static const DWORD IAT_RVA_waveInGetDevCapsA                  = 0x0088d400;

typedef BOOL (WINAPI* PFN_DeviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef HDEVINFO (WINAPI* PFN_SetupDiGetClassDevsW)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI* PFN_SetupDiGetDeviceRegistryPropertyW)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef HDEVNOTIFY (WINAPI* PFN_RegisterDeviceNotificationW)(HANDLE, LPVOID, DWORD);
typedef MMRESULT (WINAPI* PFN_waveInOpen)(LPHWAVEIN, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
typedef UINT (WINAPI* PFN_waveInGetNumDevs)(void);
typedef MMRESULT (WINAPI* PFN_waveInGetDevCapsA)(UINT_PTR, LPWAVEINCAPSA, UINT);

static PFN_DeviceIoControl                   s_Real_DeviceIoControl = nullptr;
static PFN_SetupDiGetClassDevsW              s_Real_SetupDiGetClassDevsW = nullptr;
static PFN_SetupDiGetDeviceRegistryPropertyW s_Real_SetupDiGetDeviceRegistryPropertyW = nullptr;
static PFN_RegisterDeviceNotificationW       s_Real_RegisterDeviceNotificationW = nullptr;
static PFN_waveInOpen                        s_Real_waveInOpen = nullptr;
static PFN_waveInGetNumDevs                  s_Real_waveInGetNumDevs = nullptr;
static PFN_waveInGetDevCapsA                 s_Real_waveInGetDevCapsA = nullptr;

static std::mutex s_DiagHandleMutex;
static std::map<HANDLE, std::wstring> s_DiagHandlePaths;
static std::map<std::pair<HANDLE, DWORD>, unsigned> s_DiagIoctlCounts;

static void DiagTrackDeviceHandle(HANDLE h, LPCWSTR path)
{
	if (h == INVALID_HANDLE_VALUE || !path || path[0] != L'\\' || path[1] != L'\\')
		return;
	std::lock_guard<std::mutex> g(s_DiagHandleMutex);
	s_DiagHandlePaths[h] = path;
	// handle values get reused after CloseHandle; reset per-handle counters
	for (auto it = s_DiagIoctlCounts.begin(); it != s_DiagIoctlCounts.end();)
	{
		if (it->first.first == h) it = s_DiagIoctlCounts.erase(it);
		else ++it;
	}
}

static std::string DiagHex(const void* data, DWORD size, DWORD maxBytes = 64)
{
	if (!data || !size) return "-";
	std::string out;
	const BYTE* p = static_cast<const BYTE*>(data);
	const DWORD n = (std::min<DWORD>)(size, maxBytes);
	char buf[4];
	for (DWORD i = 0; i < n; ++i)
	{
		sprintf_s(buf, sizeof(buf), "%02X", p[i]);
		out += buf;
		if (i + 1 < n) out += ((i + 1) % 4 == 0) ? " " : "";
	}
	if (size > maxBytes) out += " ...";
	return out;
}

// For IOCTL_KS_PROPERTY & co. the input buffer starts with a KSIDENTIFIER (GUID Set, ULONG Id, ULONG Flags)
static std::string DiagKsIdentifier(const void* in, DWORD inSize)
{
	if (!in || inSize < sizeof(GUID) + 2 * sizeof(ULONG)) return "";
	const GUID* set = static_cast<const GUID*>(in);
	const ULONG* rest = reinterpret_cast<const ULONG*>(static_cast<const BYTE*>(in) + sizeof(GUID));
	std::ostringstream os;
	os << " ks.set=" << DiagFmtGuid(set) << " ks.id=" << std::dec << rest[0] << " ks.flags=0x" << std::hex << rest[1];
	return os.str();
}

static BOOL WINAPI Diag_DeviceIoControl(HANDLE h, DWORD code, LPVOID inBuf, DWORD inSize, LPVOID outBuf, DWORD outSize, LPDWORD pReturned, LPOVERLAPPED ov)
{
	const BOOL ret = s_Real_DeviceIoControl(h, code, inBuf, inSize, outBuf, outSize, pReturned, ov);
	const DWORD gle = GetLastError();

	std::wstring path;
	unsigned count = 0;
	{
		std::lock_guard<std::mutex> g(s_DiagHandleMutex);
		auto it = s_DiagHandlePaths.find(h);
		if (it != s_DiagHandlePaths.end())
			path = it->second;
		count = ++s_DiagIoctlCounts[std::make_pair(h, code)];
	}

	// limit spam: log the first 40 calls per handle+ioctl, then every 1000th
	if (count <= 40 || (count % 1000) == 0)
	{
		const DWORD returned = (ret && pReturned) ? *pReturned : 0;
		rslog::info_ts() << "Diag_DeviceIoControl h=" << h
			<< " path=" << (path.empty() ? std::wstring(L"(unknown)") : path)
			<< " code=0x" << std::hex << code
			<< " in=" << std::dec << inSize << " out=" << outSize
			<< (ov ? " overlapped" : "")
			<< " -> " << ret << " returned=" << returned << " gle=" << WinErrStr(gle)
			<< " call#" << count
			<< DiagKsIdentifier(inBuf, inSize)
			<< " inData=" << DiagHex(inBuf, inSize)
			<< " outData=" << ((ret && !ov) ? DiagHex(outBuf, returned) : std::string("-"))
			<< std::endl;
	}

	SetLastError(gle);
	return ret;
}

static HDEVINFO WINAPI Diag_SetupDiGetClassDevsW(const GUID* classGuid, PCWSTR enumerator, HWND hwnd, DWORD flags)
{
	HDEVINFO ret = s_Real_SetupDiGetClassDevsW(classGuid, enumerator, hwnd, flags);
	const DWORD gle = GetLastError();
	rslog::info_ts() << "Diag_SetupDiGetClassDevsW classGuid=" << DiagFmtGuid(classGuid)
		<< " enumerator=" << (enumerator ? std::wstring(enumerator) : std::wstring(L"(null)"))
		<< " flags=0x" << std::hex << flags << " -> " << ret << " gle=" << WinErrStr(gle) << std::endl;
	SetLastError(gle);
	return ret;
}

static BOOL WINAPI Diag_SetupDiGetDeviceRegistryPropertyW(HDEVINFO devs, PSP_DEVINFO_DATA devInfo, DWORD prop, PDWORD pRegType, PBYTE buf, DWORD bufSize, PDWORD pRequired)
{
	DWORD regType = 0;
	const BOOL ret = s_Real_SetupDiGetDeviceRegistryPropertyW(devs, devInfo, prop, &regType, buf, bufSize, pRequired);
	const DWORD gle = GetLastError();
	if (pRegType) *pRegType = regType;

	auto& log = rslog::info_ts();
	log << "Diag_SetupDiGetDeviceRegistryPropertyW prop=" << std::dec << prop
		<< " bufSize=" << bufSize << " -> " << ret << " gle=" << WinErrStr(gle) << " regType=" << regType;
	if (ret && buf && (regType == REG_SZ || regType == REG_MULTI_SZ || regType == REG_EXPAND_SZ))
	{
		// print the first string (and following ones for MULTI_SZ)
		const wchar_t* s = reinterpret_cast<const wchar_t*>(buf);
		const wchar_t* end = reinterpret_cast<const wchar_t*>(buf + bufSize);
		log << " value=";
		while (s < end && *s)
		{
			log << "\"" << std::wstring(s) << "\" ";
			s += wcslen(s) + 1;
			if (regType != REG_MULTI_SZ) break;
		}
	}
	else if (ret && buf)
	{
		log << " data=" << DiagHex(buf, pRequired ? *pRequired : bufSize, 32);
	}
	log << std::endl;

	SetLastError(gle);
	return ret;
}

static HDEVNOTIFY WINAPI Diag_RegisterDeviceNotificationW(HANDLE recipient, LPVOID filter, DWORD flags)
{
	HDEVNOTIFY ret = s_Real_RegisterDeviceNotificationW(recipient, filter, flags);
	const DWORD gle = GetLastError();
	auto& log = rslog::info_ts();
	log << "Diag_RegisterDeviceNotificationW flags=0x" << std::hex << flags;
	if (filter)
	{
		const DEV_BROADCAST_HDR* hdr = static_cast<const DEV_BROADCAST_HDR*>(filter);
		log << " devicetype=" << std::dec << hdr->dbch_devicetype;
		if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && hdr->dbch_size >= sizeof(DEV_BROADCAST_DEVICEINTERFACE_W))
			log << " classGuid=" << DiagFmtGuid(&static_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(filter)->dbcc_classguid);
	}
	log << " -> " << ret << " gle=" << WinErrStr(gle) << std::endl;
	SetLastError(gle);
	return ret;
}

static UINT WINAPI Diag_waveInGetNumDevs(void)
{
	const UINT ret = s_Real_waveInGetNumDevs();
	rslog::info_ts() << "Diag_waveInGetNumDevs -> " << std::dec << ret << std::endl;
	return ret;
}

static MMRESULT WINAPI Diag_waveInGetDevCapsA(UINT_PTR devId, LPWAVEINCAPSA caps, UINT capsSize)
{
	const MMRESULT ret = s_Real_waveInGetDevCapsA(devId, caps, capsSize);
	auto& log = rslog::info_ts();
	log << "Diag_waveInGetDevCapsA dev=" << std::dec << devId << " -> " << ret;
	if (ret == MMSYSERR_NOERROR && caps)
		log << " name=\"" << caps->szPname << "\" mid=" << caps->wMid << " pid=" << caps->wPid << " channels=" << caps->wChannels;
	log << std::endl;
	return ret;
}

static MMRESULT WINAPI Diag_waveInOpen(LPHWAVEIN phwi, UINT devId, LPCWAVEFORMATEX fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
	const MMRESULT ret = s_Real_waveInOpen(phwi, devId, fmt, cb, inst, flags);
	auto& log = rslog::info_ts();
	log << "Diag_waveInOpen dev=" << std::dec << devId << " flags=0x" << std::hex << flags << " -> " << std::dec << ret;
	if (fmt)
		log << " fmt: tag=" << fmt->wFormatTag << " ch=" << fmt->nChannels << " rate=" << fmt->nSamplesPerSec << " bits=" << fmt->wBitsPerSample;
	log << std::endl;
	return ret;
}

static HANDLE WINAPI Diag_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSA, DWORD dwCD, DWORD dwFlags, HANDLE hTemplate)
{
	if (IsFakeRsAsioCablePath(lpFileName))
	{
		// Our synthetic RS_ASIO entry has no real KS device behind it.
		// Return a null-device handle so the game gets a valid, closeable handle.
		// The game will likely fail its IOCTL check on this handle and move on to
		// the real winepipewire-registered cable path (vid_12ba...REAL_INSTANCE).
		HANDLE hDummy = s_Real_CreateFileW(L"\\\\.\\NUL", 0,
		                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
		                                    nullptr, OPEN_EXISTING, 0, nullptr);
		rslog::info_ts() << "Patched_CreateFileW (fake RS_ASIO path -> NUL): "
		                 << std::wstring(lpFileName)
		                 << " access=0x" << std::hex << dwDesiredAccess
		                 << " -> " << hDummy << " gle=" << WinErrStr(GetLastError())
		                 << " [synthetic entry redirected to NUL; game will issue an IOCTL that fails"
		                    " gracefully, then move to the next cable interface]" << std::endl;
		return hDummy;
	}

	HANDLE ret = s_Real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSA, dwCD, dwFlags, hTemplate);
	const DWORD createGle = GetLastError();
	DiagTrackDeviceHandle(ret, lpFileName);
	SetLastError(createGle);

	// Log all device-like paths (\\?\ or \\.\) including the real cable path from winepipewire.
	if (IsCableDevicePath(lpFileName) || (lpFileName && lpFileName[0] == L'\\' && lpFileName[1] == L'\\'))
	{
		rslog::info_ts() << "Patched_CreateFileW: "
		                 << std::wstring(lpFileName)
		                 << " access=0x" << std::hex << dwDesiredAccess
		                 << " -> " << ret << " gle=" << WinErrStr(GetLastError());
		if (ret == INVALID_HANDLE_VALUE && IsCableDevicePath(lpFileName))
			rslog::info_ts() << " [WARN: KS device open FAILED - Wine has no KS driver for this path;"
		                    " expected under winepipewire - cable detection depends on WASAPI properties instead]";
		rslog::info_ts() << std::endl;
	}
	return ret;
}

// ---------------------------------------------------------------------------
// Ensure the Real Tone Cable (USB VID_12BA:PID_00FF) is registered in Wine's
// KS device class registry.
//
// Newer Wine/Proton no longer automatically populates these entries when a USB
// audio device is connected via PipeWire. Without them, the game's SetupAPI
// cable-presence scan (SetupDiGetClassDevsA for KSCATEGORY_AUDIO) returns no
// results and the game never opens guitar input.
//
// The cable's USB product string is "Rocksmith USB Guitar Adapter" — this is
// the value Wine's old USB audio driver stored as the KS interface FriendlyName
// and is what the game's cable-detection code expects to find.
//
// We create the entries if they are absent; if they already exist (old Wine
// version) we leave them alone. The "RS_ASIO" suffix is used as the device
// instance ID so we can identify our own entries unambiguously.
// ---------------------------------------------------------------------------

static bool IsRunningUnderWine()
{
	// Wine's ntdll exports wine_get_version; native Windows' ntdll does not.
	const HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
	return hNtdll && GetProcAddress(hNtdll, "wine_get_version") != nullptr;
}

static void EnsureRealToneCableRegistered()
{
	rslog::info_ts() << "EnsureRealToneCableRegistered - checking KS device class registrations..." << std::endl;

	// USB product string for VID_12BA:00FF (confirmed via lsusb iProduct field)
	static const wchar_t kFriendlyName[]   = L"Rocksmith USB Guitar Adapter";
	static const wchar_t kDeviceInstance[] = L"USB\\VID_12BA&PID_00FF\\RS_ASIO";
	static const wchar_t kInstanceSuffix[] = L"RS_ASIO";

	// KS class GUIDs the game's cable-detection scan queries:
	//   {6994AD04-...} = KSCATEGORY_AUDIO          (primary scan)
	//   {65E8773E-...} = KSCATEGORY_AUDIO_DEVICE   (alias query 1)
	//   {65E8773D-...} = KSCATEGORY_AUDIO_CONTROL  (alias query 2)
	//   {EB115FFC-...} = KSCATEGORY_WDMAUD         (alias query 3, second loop)
	static const wchar_t* kClassGuids[] = {
		L"{6994AD04-93EF-11D0-A3CC-00A0C9223196}",
		L"{65E8773E-8F56-11D0-A3B9-00A0C9223196}",
		L"{65E8773D-8F56-11D0-A3B9-00A0C9223196}",
		L"{EB115FFC-10C8-4964-831D-6DCB02E6F23F}",
	};

	// Wine's SETUPDI_EnumerateMatchingInterfaces reads the DeviceInstance value from the
	// DeviceClasses interface key and then looks up that instance in
	// HKLM\SYSTEM\CurrentControlSet\Enum.  If the Enum entry is absent the interface is
	// never surfaced, regardless of what exists under DeviceClasses.
	// Create the Enum device node (USB\VID_12BA&PID_00FF\RS_ASIO) if it is missing.
	{
		static const wchar_t kEnumKey[] =
			L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_12BA&PID_00FF\\RS_ASIO";
		// Use KSCATEGORY_AUDIO as the device class GUID (the value is arbitrary for
		// our purposes; Wine only requires a valid GUID string to call create_device).
		static const wchar_t kClassGuid[] = L"{6994AD04-93EF-11D0-A3CC-00A0C9223196}";
		HKEY hKey = nullptr;
		DWORD disp = 0;
		LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kEnumKey, 0, nullptr,
		                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, &disp);
		if (rc == ERROR_SUCCESS)
		{
			if (disp == REG_CREATED_NEW_KEY)
			{
				RegSetValueExW(hKey, L"ClassGUID", 0, REG_SZ,
				               (const BYTE*)kClassGuid,
				               (DWORD)((wcslen(kClassGuid) + 1) * sizeof(wchar_t)));
				RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ,
				               (const BYTE*)kFriendlyName,
				               (DWORD)((wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
				rslog::info_ts() << "  created Enum device node" << std::endl;
			}
			RegCloseKey(hKey);
		}
		else
		{
			rslog::error_ts() << "  RegCreateKeyExW failed for Enum device node (rc=" << rc << ")" << std::endl;
		}
	}

	int created = 0;
	for (const auto* guid : kClassGuids)
	{
		// Registry key under DeviceClasses for this interface:
		//   SYSTEM\...\DeviceClasses\{GUID}\##?#USB#VID_12BA&PID_00FF#RS_ASIO#{GUID}
		// (Windows convention: \\?\ → ##?# and each path separator → #)
		std::wstring wGuid = guid;
		std::wstring ifaceKey = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses\\") +
		                        wGuid + L"\\##?#USB#VID_12BA&PID_00FF#" + kInstanceSuffix + L"#" + wGuid;
		std::wstring instanceKey = ifaceKey + L"\\#";
		std::wstring controlKey  = instanceKey + L"\\Control";
		std::wstring paramsKey   = instanceKey + L"\\Device Parameters";

		// SymbolicLink value: lowercase device path (Windows convention)
		std::wstring symlink = std::wstring(L"\\\\?\\USB#VID_12BA&PID_00FF#") + kInstanceSuffix + L"#" + wGuid;
		std::transform(symlink.begin(), symlink.end(), symlink.begin(),
		               [](wchar_t c) { return (wchar_t)::towlower(c); });

		// Skip only if the \#\Control\Linked value already exists.
		// Wine's is_linked() checks exactly this; without it DIGCF_PRESENT will skip
		// the interface even if the \# subkey exists.
		{
			HKEY hCheck = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, controlKey.c_str(), 0, KEY_READ, &hCheck) == ERROR_SUCCESS)
			{
				DWORD linked = 0, type = 0, size = sizeof(linked);
				bool hasLinked = (RegQueryValueExW(hCheck, L"Linked", nullptr, &type,
				                                   (BYTE*)&linked, &size) == ERROR_SUCCESS
				                  && type == REG_DWORD && linked != 0);
				RegCloseKey(hCheck);
				if (hasLinked)
				{
					rslog::info_ts() << "  already registered (Control\\Linked present): " << wGuid << std::endl;
					continue;
				}
			}
		}

		// Create/open interface key with SymbolicLink and DeviceInstance values
		{
			HKEY hKey = nullptr;
			DWORD disp = 0;
			LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ifaceKey.c_str(), 0, nullptr,
			                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, &disp);
			if (rc != ERROR_SUCCESS)
			{
				rslog::error_ts() << "  RegCreateKeyExW failed for interface key " << wGuid
				                  << " (rc=" << rc << ")" << std::endl;
				continue;
			}
			RegSetValueExW(hKey, L"SymbolicLink", 0, REG_SZ,
			               (const BYTE*)symlink.c_str(),
			               (DWORD)((symlink.size() + 1) * sizeof(wchar_t)));
			RegSetValueExW(hKey, L"DeviceInstance", 0, REG_SZ,
			               (const BYTE*)kDeviceInstance,
			               (DWORD)((wcslen(kDeviceInstance) + 1) * sizeof(wchar_t)));
			RegCloseKey(hKey);
		}

		// Create/open \# instance subkey with SymbolicLink
		{
			HKEY hKey = nullptr;
			DWORD disp = 0;
			LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, instanceKey.c_str(), 0, nullptr,
			                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, &disp);
			if (rc != ERROR_SUCCESS)
			{
				rslog::error_ts() << "  RegCreateKeyExW failed for instance key " << wGuid
				                  << " (rc=" << rc << ")" << std::endl;
				continue;
			}
			RegSetValueExW(hKey, L"SymbolicLink", 0, REG_SZ,
			               (const BYTE*)symlink.c_str(),
			               (DWORD)((symlink.size() + 1) * sizeof(wchar_t)));
			RegCloseKey(hKey);
		}

		// Create \#\Control with Linked=1.
		// Wine's is_linked() reads this to decide if the interface is PRESENT
		// (DIGCF_PRESENT / SPINT_ACTIVE).  Without it SetupDiEnumDeviceInterfaces
		// returns ERROR_NO_MORE_ITEMS even though the \# subkey exists.
		{
			HKEY hKey = nullptr;
			DWORD disp = 0;
			LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, controlKey.c_str(), 0, nullptr,
			                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, &disp);
			if (rc != ERROR_SUCCESS)
			{
				rslog::error_ts() << "  RegCreateKeyExW failed for Control key " << wGuid
				                  << " (rc=" << rc << ")" << std::endl;
				continue;
			}
			DWORD linked = 1;
			RegSetValueExW(hKey, L"Linked", 0, REG_DWORD,
			               (const BYTE*)&linked, sizeof(linked));
			RegCloseKey(hKey);
			++created;
		}

		// Create \#\Device Parameters with FriendlyName
		{
			HKEY hKey = nullptr;
			DWORD disp = 0;
			LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, paramsKey.c_str(), 0, nullptr,
			                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, &disp);
			if (rc == ERROR_SUCCESS)
			{
				RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ,
				               (const BYTE*)kFriendlyName,
				               (DWORD)((wcslen(kFriendlyName) + 1) * sizeof(wchar_t)));
				RegCloseKey(hKey);
			}
			else
			{
				rslog::error_ts() << "  RegCreateKeyExW failed for params key " << wGuid
				                  << " (rc=" << rc << ")" << std::endl;
			}
		}
	}

	rslog::info_ts() << "EnsureRealToneCableRegistered: " << created << " KS interface(s) created";
	if (created == 0)
		rslog::info_ts() << " [all 4 registrations already present in registry]"
		                    " [if cable is still not detected, the issue is in WASAPI properties, not KS registry]";
	else
		rslog::info_ts() << " [new entries written to registry - SetupDiGetClassDevsA should return them now]";
	rslog::info_ts() << std::endl;
}

// Patch code for Rocksmith (2011), CRC32 0xe0f686e0
//
// The Rocksmith 2011 executable encrypts its .text section on disk using a custom
// packer stub (PSFD00 section), so call-site byte patterns cannot be found statically.
// Instead, we patch the Import Address Table (IAT) directly.
//
// The IAT is in .rdata (unencrypted) and is filled by the loader before our DLL runs.
// We overwrite the function pointer slots for the COM functions that PortAudio/WASAPI
// uses to enumerate audio devices, replacing them with our own implementations.
//
// IAT entry RVAs (relative to image base 0x00400000, verified from the PE headers):
//
//   CoCreateInstance                      RVA 0x0088d47c  (ole32.dll)
//   CoMarshalInterThreadInterfaceInStream  RVA 0x0088d490  (ole32.dll)
//   CoGetInterfaceAndReleaseStream         RVA 0x0088d494  (ole32.dll)

// Known IAT RVAs - these are fixed offsets from the module base and do not change
// because the executable does not use ASLR (ImageBase is fixed at 0x00400000).
static const DWORD IAT_RVA_CoCreateInstance                     = 0x0088d47c;
static const DWORD IAT_RVA_CoMarshalInterThreadInterfaceInStream = 0x0088d490;
static const DWORD IAT_RVA_CoGetInterfaceAndReleaseStream        = 0x0088d494;

// Patch a single IAT slot to point to replacementFn.
// Patch_ReplaceWithBytes (from Patcher.h) handles page protection internally
// via NtProtectVirtualMemory, bypassing any hook on VirtualProtect.
static bool PatchIATEntry(DWORD rva, void* replacementFn, const char* fnName)
{
	const HMODULE hBase = GetModuleHandle(NULL);
	if (!hBase)
	{
		rslog::error_ts() << "PatchIATEntry: GetModuleHandle failed for " << fnName << std::endl;
		return false;
	}

	void** iatSlot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(hBase) + rva);

	rslog::info_ts() << "PatchIATEntry: patching " << fnName
	                 << " at " << iatSlot
	                 << "  old=" << *iatSlot
	                 << "  new=" << replacementFn << std::endl;

	Patch_ReplaceWithBytes(iatSlot, sizeof(void*), reinterpret_cast<const BYTE*>(&replacementFn));
	return true;
}

// Thin wrappers that match the calling convention of the ole32 functions we are replacing.
// CoMarshalInterThreadInterfaceInStream / CoGetInterfaceAndReleaseStream are patched to
// no-op / identity replacements so that PortAudio's cross-thread COM marshaling does not
// interfere with our fake WASAPI device objects.
//
// NOTE: These are only used if RS2011's embedded PortAudio performs cross-apartment
// marshaling the same way RS2014's does. The game may work without them; if it crashes
// after CoCreateInstance is redirected successfully, these patches are the next step.

static HRESULT STDAPICALLTYPE Patched_CoMarshalInterThreadInterfaceInStream(
    REFIID riid, IUnknown* pUnk, IStream** ppStm)
{
	rslog::info_ts() << "Patched_CoMarshalInterThreadInterfaceInStream called" << std::endl;
	// Return the interface directly as the "stream" — PortAudio will read it back
	// via Patched_CoGetInterfaceAndReleaseStream below.
	if (!ppStm)
		return E_POINTER;
	*ppStm = reinterpret_cast<IStream*>(pUnk);
	if (pUnk)
		pUnk->AddRef();
	return S_OK;
}

static HRESULT STDAPICALLTYPE Patched_CoGetInterfaceAndReleaseStream(
    IStream* pStm, REFIID riid, void** ppv)
{
	rslog::info_ts() << "Patched_CoGetInterfaceAndReleaseStream called" << std::endl;
	if (!ppv)
		return E_POINTER;
	// The "stream" is actually the interface pointer stored by Patched_CoMarshalInterThreadInterfaceInStream.
	// The real CoGetInterfaceAndReleaseStream deserializes and returns the pointer that was originally
	// marshaled — it does NOT call QueryInterface on the result (the stream data already encodes the
	// correct interface type). We replicate that: just hand back the stored pointer directly.
	// The AddRef performed during marshal transfers to the caller; no additional AddRef or Release needed.
	IUnknown* pUnk = reinterpret_cast<IUnknown*>(pStm);
	if (!pUnk)
	{
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	*ppv = pUnk;
	return S_OK;
}

void PatchOriginalCode_e0f686e0()
{
	rslog::info_ts() << __FUNCTION__ << " - patching Rocksmith 2011 via IAT" << std::endl;

	// Wine/Proton regression: newer versions no longer auto-register USB audio devices
	// in the KS device class registry. Create the missing entries so that the game's
	// SetupAPI cable-presence check finds the Real Tone Cable.
	// On native Windows the OS registers the cable itself, and writing to HKLM needs
	// admin rights anyway (it fails with ERROR_ACCESS_DENIED), so only do this under Wine.
	if (IsRunningUnderWine())
	{
		EnsureRealToneCableRegistered();
	}
	else
	{
		rslog::info_ts() << __FUNCTION__ << " - native Windows detected, skipping KS registry fix-up (Wine only)" << std::endl;
	}

	// Capture original function pointers from the IAT before we overwrite the slots.
	// The diagnostic wrappers call through these to preserve real behaviour.
	{
		const HMODULE hMod = GetModuleHandle(NULL);
		auto ReadSlot = [hMod](DWORD rva) -> void* {
			return *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(hMod) + rva);
		};
		s_Real_SetupDiGetClassDevsA          = (PFN_SetupDiGetClassDevsA)         ReadSlot(IAT_RVA_SetupDiGetClassDevsA);
		s_Real_SetupDiEnumDeviceInterfaces   = (PFN_SetupDiEnumDeviceInterfaces)  ReadSlot(IAT_RVA_SetupDiEnumDeviceInterfaces);
		s_Real_SetupDiGetDeviceInterfaceDetailW = (PFN_SetupDiGetDeviceInterfaceDetailW)ReadSlot(IAT_RVA_SetupDiGetDeviceInterfaceDetailW);
		s_Real_SetupDiGetDeviceInterfaceAlias   = (PFN_SetupDiGetDeviceInterfaceAlias)  ReadSlot(IAT_RVA_SetupDiGetDeviceInterfaceAlias);
		s_Real_SetupDiOpenDeviceInterfaceRegKey = (PFN_SetupDiOpenDeviceInterfaceRegKey)ReadSlot(IAT_RVA_SetupDiOpenDeviceInterfaceRegKey);
		s_Real_SetupDiDestroyDeviceInfoList  = (PFN_SetupDiDestroyDeviceInfoList) ReadSlot(IAT_RVA_SetupDiDestroyDeviceInfoList);
		s_Real_CreateFileW                   = (PFN_CreateFileW)                  ReadSlot(IAT_RVA_CreateFileW);
		s_Real_DeviceIoControl               = (PFN_DeviceIoControl)              ReadSlot(IAT_RVA_DeviceIoControl);
		s_Real_SetupDiGetClassDevsW          = (PFN_SetupDiGetClassDevsW)         ReadSlot(IAT_RVA_SetupDiGetClassDevsW);
		s_Real_SetupDiGetDeviceRegistryPropertyW = (PFN_SetupDiGetDeviceRegistryPropertyW)ReadSlot(IAT_RVA_SetupDiGetDeviceRegistryPropertyW);
		s_Real_RegisterDeviceNotificationW   = (PFN_RegisterDeviceNotificationW)  ReadSlot(IAT_RVA_RegisterDeviceNotificationW);
		s_Real_waveInOpen                    = (PFN_waveInOpen)                   ReadSlot(IAT_RVA_waveInOpen);
		s_Real_waveInGetNumDevs              = (PFN_waveInGetNumDevs)             ReadSlot(IAT_RVA_waveInGetNumDevs);
		s_Real_waveInGetDevCapsA             = (PFN_waveInGetDevCapsA)            ReadSlot(IAT_RVA_waveInGetDevCapsA);
	}

	bool ok = true;

	ok &= PatchIATEntry(IAT_RVA_CoCreateInstance,
	                    reinterpret_cast<void*>(&Patched_CoCreateInstance),
	                    "CoCreateInstance");

	ok &= PatchIATEntry(IAT_RVA_CoMarshalInterThreadInterfaceInStream,
	                    reinterpret_cast<void*>(&Patched_CoMarshalInterThreadInterfaceInStream),
	                    "CoMarshalInterThreadInterfaceInStream");

	ok &= PatchIATEntry(IAT_RVA_CoGetInterfaceAndReleaseStream,
	                    reinterpret_cast<void*>(&Patched_CoGetInterfaceAndReleaseStream),
	                    "CoGetInterfaceAndReleaseStream");

	// Diagnostic-only hooks: pass through to real functions, log inputs/results.
	PatchIATEntry(IAT_RVA_SetupDiGetClassDevsA,           reinterpret_cast<void*>(&Diag_SetupDiGetClassDevsA),           "SetupDiGetClassDevsA");
	PatchIATEntry(IAT_RVA_SetupDiEnumDeviceInterfaces,    reinterpret_cast<void*>(&Diag_SetupDiEnumDeviceInterfaces),    "SetupDiEnumDeviceInterfaces");
	PatchIATEntry(IAT_RVA_SetupDiGetDeviceInterfaceDetailW,reinterpret_cast<void*>(&Diag_SetupDiGetDeviceInterfaceDetailW),"SetupDiGetDeviceInterfaceDetailW");
	PatchIATEntry(IAT_RVA_SetupDiGetDeviceInterfaceAlias, reinterpret_cast<void*>(&Diag_SetupDiGetDeviceInterfaceAlias), "SetupDiGetDeviceInterfaceAlias");
	PatchIATEntry(IAT_RVA_SetupDiOpenDeviceInterfaceRegKey,reinterpret_cast<void*>(&Diag_SetupDiOpenDeviceInterfaceRegKey),"SetupDiOpenDeviceInterfaceRegKey");
	PatchIATEntry(IAT_RVA_SetupDiDestroyDeviceInfoList,   reinterpret_cast<void*>(&Diag_SetupDiDestroyDeviceInfoList),   "SetupDiDestroyDeviceInfoList");
	PatchIATEntry(IAT_RVA_CreateFileW,                    reinterpret_cast<void*>(&Diag_CreateFileW),                    "CreateFileW");
	PatchIATEntry(IAT_RVA_DeviceIoControl,                reinterpret_cast<void*>(&Diag_DeviceIoControl),                "DeviceIoControl");
	PatchIATEntry(IAT_RVA_SetupDiGetClassDevsW,           reinterpret_cast<void*>(&Diag_SetupDiGetClassDevsW),           "SetupDiGetClassDevsW");
	PatchIATEntry(IAT_RVA_SetupDiGetDeviceRegistryPropertyW, reinterpret_cast<void*>(&Diag_SetupDiGetDeviceRegistryPropertyW), "SetupDiGetDeviceRegistryPropertyW");
	PatchIATEntry(IAT_RVA_RegisterDeviceNotificationW,    reinterpret_cast<void*>(&Diag_RegisterDeviceNotificationW),    "RegisterDeviceNotificationW");
	PatchIATEntry(IAT_RVA_waveInOpen,                     reinterpret_cast<void*>(&Diag_waveInOpen),                     "waveInOpen");
	PatchIATEntry(IAT_RVA_waveInGetNumDevs,               reinterpret_cast<void*>(&Diag_waveInGetNumDevs),               "waveInGetNumDevs");
	PatchIATEntry(IAT_RVA_waveInGetDevCapsA,              reinterpret_cast<void*>(&Diag_waveInGetDevCapsA),              "waveInGetDevCapsA");

	if (!ok)
	{
		rslog::error_ts() << __FUNCTION__ << " - one or more IAT patches failed" << std::endl;
	}
	else
	{
		rslog::info_ts() << __FUNCTION__ << " - all IAT patches applied successfully" << std::endl;
	}
}

