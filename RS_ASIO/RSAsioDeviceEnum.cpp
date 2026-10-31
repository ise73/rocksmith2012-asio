#include "stdafx.h"
#include "RSAsioDeviceEnum.h"
#include "RSAsioDevice.h"
#include "AsioHelpers.h"
#include "AsioSharedHost.h"
#include <setupapi.h>

#pragma comment(lib, "setupapi.lib")

bool g_EmulateRealToneCable = false;

// True if a real Real Tone Cable (USB VID_12BA:PID_00FF) is currently plugged in.
// Uses SetupAPI directly from this DLL, so the game's hooked IAT entries are not involved.
static bool IsRealToneCableConnected()
{
	static const GUID kKsCategoryAudio = { 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

	HDEVINFO set = SetupDiGetClassDevsW(&kKsCategoryAudio, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE)
		return false;

	bool found = false;
	std::vector<BYTE> buf(2048);
	for (DWORD i = 0; !found; ++i)
	{
		SP_DEVICE_INTERFACE_DATA ifd{};
		ifd.cbSize = sizeof(ifd);
		if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kKsCategoryAudio, i, &ifd))
			break;

		auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, (DWORD)buf.size(), nullptr, nullptr))
		{
			std::wstring p(detail->DevicePath);
			std::transform(p.begin(), p.end(), p.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
			found = p.find(L"vid_12ba&pid_00ff") != std::wstring::npos;
		}
	}

	SetupDiDestroyDeviceInfoList(set);
	return found;
}

// ---------------------------------------------------------------------------
// WASAPI device ID → ASIO-backed IMMDevice redirect registry
// Populated at startup when [Asio.InputN] WasapiDevice= is configured.
// ---------------------------------------------------------------------------
struct WasapiRedirectEntry { std::wstring subId; IMMDevice* device; };
static std::vector<WasapiRedirectEntry> s_wasapiRedirects;

void RegisterWasapiRedirect(const std::wstring& wasapiDeviceSubId, IMMDevice* asioDevice)
{
	if (!asioDevice || wasapiDeviceSubId.empty())
		return;
	asioDevice->AddRef();
	// Pre-lowercase so GetWasapiRedirectDevice can compare with a simple find().
	std::wstring lowerSubId = wasapiDeviceSubId;
	std::transform(lowerSubId.begin(), lowerSubId.end(), lowerSubId.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
	s_wasapiRedirects.push_back({ lowerSubId, asioDevice });
	rslog::info_ts() << "RegisterWasapiRedirect: " << wasapiDeviceSubId << std::endl;
}

void ClearWasapiRedirects()
{
	for (auto& entry : s_wasapiRedirects)
		entry.device->Release();
	s_wasapiRedirects.clear();
}

IMMDevice* GetWasapiRedirectDevice(const std::wstring& deviceId, const std::wstring& deviceFriendlyName)
{
	if (s_wasapiRedirects.empty())
		return nullptr;

	// Lowercase once before the loop; entries already store pre-lowercased subIds.
	auto toLower = [](std::wstring s) {
		std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
		return s;
	};
	const std::wstring lowerDeviceId = toLower(deviceId);
	const std::wstring lowerFriendlyName = toLower(deviceFriendlyName);

	for (auto& entry : s_wasapiRedirects)
	{
		if (lowerDeviceId.find(entry.subId) != std::wstring::npos)
			return entry.device;

		if (!lowerFriendlyName.empty() && lowerFriendlyName.find(entry.subId) != std::wstring::npos)
			return entry.device;
	}
	return nullptr;
}

void RSAsioDeviceEnum::SetConfig(const RSAsioConfig& config)
{
	m_DeviceListNeedsUpdate = true;
	m_Config = config;
}

void RSAsioDeviceEnum::UpdateAvailableDevices()
{
	m_DeviceListNeedsUpdate = false;
	ClearWasapiRedirects();

	ClearAll();

	std::vector<AsioHelpers::DriverInfo> asioDriversInfo = AsioHelpers::FindDrivers();

	std::vector<AsioSharedHost*> createdAsioHosts;
	auto fnFindOrCreateAsioHost = [&](const std::string& name) -> AsioSharedHost*
	{
		for (const auto& info : asioDriversInfo)
		{
			if (info.Name == name)
			{
				// check if we already have this host created
				for (const auto& host : createdAsioHosts)
				{
					if (host->GetAsioDllPath() == info.DllPath)
					{
						host->AddRef();
						return host;
					}
				}

				// if we got here, we don't have it created yet and we'll create it now
				AsioSharedHost* newHost = AsioHelpers::CreateAsioHost(info);
				if (newHost)
				{
					createdAsioHosts.push_back(newHost);
					newHost->AddRef();
					return newHost;
				}
			}
		}

		return nullptr;
	};
	
	if (!m_Config.output.asioDriverName.empty())
	{
		rslog::info_ts() << __FUNCTION__ << " - " << "output requesting ASIO driver: " << m_Config.output.asioDriverName << std::endl;

		AsioSharedHost* host = fnFindOrCreateAsioHost(m_Config.output.asioDriverName);
		if (host)
		{
			RSAsioDevice::Config config;
			config.isOutput = true;
			config.baseAsioChannelNumber = m_Config.output.baseChannel;
			config.altOutputBaseAsioChannelNumber = m_Config.output.altBaseChannel;
			config.numAsioChannels = m_Config.output.numChannels;
			config.bufferSizeMode = m_Config.bufferMode;
			config.customBufferSize = m_Config.customBufferSize;
			config.enablePollingRingBuffer = m_Config.enablePollingRingBuffer;
			config.pollingBufferSize = m_Config.pollingBufferSize;
			config.enableSoftwareEndpointVolmeControl = m_Config.output.enableSoftwareEndpointVolumeControl;
			config.enableSoftwareMasterVolumeControl = m_Config.output.enableSoftwareMasterVolumeControl;
			config.enableRefCountHack = m_Config.output.enableRefCountHack;

			auto device = new RSAsioDevice(*host, L"{ASIO Out}", config);
			device->SetMasterVolumeLevelScalar((float)m_Config.output.softwareMasterVolumePercent / 100.0f);
			m_RenderDevices.AddDevice(device);
			device->Release();
			device = nullptr;

			rslog::info_ts() << __FUNCTION__ << " - OK" << std::endl;
			host->Release();
		}
		else
		{
			rslog::error_ts() << __FUNCTION__ << " - output ASIO driver '" << m_Config.output.asioDriverName
			                  << "' not found or failed to load; check RS_ASIO.ini [Output] AsioDriver= name"
			                     " matches a driver listed above and its DLL is installed" << std::endl;
		}
	}

	int inputIdx = 0;
	for (const RSAsioInputConfig& inputCfg : m_Config.inputs)
	{
		if (!inputCfg.asioDriverName.empty())
		{
			rslog::info_ts() << __FUNCTION__ << " - " << "input[" << inputIdx << "] requesting ASIO driver: " << inputCfg.asioDriverName << std::endl;

			AsioSharedHost* host = fnFindOrCreateAsioHost(inputCfg.asioDriverName);
			if (host)
			{
				wchar_t id[64]{};
				swprintf(id, 63, L"{ASIO IN %i}", inputIdx);

				RSAsioDevice::Config config;
				config.isOutput = false;
				config.baseAsioChannelNumber = inputCfg.useChannel;
				config.numAsioChannels = 1;
				config.bufferSizeMode = m_Config.bufferMode;
				config.customBufferSize = m_Config.customBufferSize;
				config.enablePollingRingBuffer = m_Config.enablePollingRingBuffer;
				config.pollingBufferSize = m_Config.pollingBufferSize;
				config.enableSoftwareEndpointVolmeControl = inputCfg.enableSoftwareEndpointVolumeControl;
				config.enableSoftwareMasterVolumeControl = inputCfg.enableSoftwareMasterVolumeControl;
				config.isMicrophone = inputCfg.microphone;
				config.emulateRealToneCable = false;
				if (inputCfg.emulateRealToneCable)
				{
					// A real cable plus an emulated one makes the game complain about too many
					// instruments, so emulation only kicks in when no real cable is plugged in.
					static const bool realCableConnected = IsRealToneCableConnected();
					if (realCableConnected)
					{
						rslog::info_ts() << __FUNCTION__ << " - input[" << inputIdx << "] EmulateRealToneCable ignored: a real Real Tone Cable is connected" << std::endl;
					}
					else
					{
						config.emulateRealToneCable = true;
						g_EmulateRealToneCable = true;
						rslog::info_ts() << __FUNCTION__ << " - input[" << inputIdx << "] will be presented to the game as a Real Tone Cable" << std::endl;
					}
				}
				config.enableRefCountHack = inputCfg.enableRefCountHack;

				auto device = new RSAsioDevice(*host, id, config);
				device->SetMasterVolumeLevelScalar((float)inputCfg.softwareMasterVolumePercent / 100.0f);

				// Register WASAPI redirect if configured
				if (!inputCfg.wasapiRedirectId.empty())
				{
					const std::wstring wRedirectId(inputCfg.wasapiRedirectId.begin(), inputCfg.wasapiRedirectId.end());
					RegisterWasapiRedirect(wRedirectId, device);
				}

				m_CaptureDevices.AddDevice(device);
				device->Release();
				device = nullptr;

				rslog::info_ts() << __FUNCTION__ << " - OK" << std::endl;
				host->Release();
			}
			else
			{
				rslog::error_ts() << __FUNCTION__ << " - input[" << inputIdx << "] ASIO driver '"
				                  << inputCfg.asioDriverName
				                  << "' not found or failed to load; check RS_ASIO.ini [Asio.InputN] AsioDriver= name"
				                     " matches a driver listed above and its DLL is installed" << std::endl;
			}
		}

		++inputIdx;
	}

	// clear up
	for (auto host : createdAsioHosts)
		host->Release();
	createdAsioHosts.clear();

	// fill defaults
	for (IMMDevice* dev : m_RenderDevices)
	{
		for (IMMDevice*& defaultDev : m_DefaultRenderDevices)
		{
			defaultDev = dev;
			defaultDev->AddRef();
		}
		break;
	}
	for (IMMDevice* dev : m_CaptureDevices)
	{
		for (IMMDevice*& defaultDev : m_DefaultCaptureDevices)
		{
			defaultDev = dev;
			defaultDev->AddRef();
		}
		break;
	}
}
