#pragma once

// Simple single-buffer FIFO for interleaved PCM frames.
// Not thread-safe by itself: callers must hold a lock while using it.
//
// Used by RSAsioAudioClient in WASAPI *polling* mode (no event callback), which is
// what Rocksmith 2011 uses. In that mode the game wakes up on its own schedule
// (~every 10ms) and writes/reads as many frames as fit, while the ASIO driver
// consumes/produces small fixed-size blocks (e.g. 128 frames). The FIFO decouples
// the two so the ASIO buffer can be much smaller than the game's polling period.
class PcmFifo
{
public:
	void Init(DWORD frameBytes, DWORD capacityFrames)
	{
		m_FrameBytes = frameBytes;
		m_CapacityFrames = capacityFrames;
		m_Data.assign((size_t)frameBytes * capacityFrames, 0);
		Clear();
	}

	void Clear()
	{
		m_ReadFrame = 0;
		m_FillFrames = 0;
	}

	DWORD GetCapacityFrames() const { return m_CapacityFrames; }
	DWORD GetFillFrames() const { return m_FillFrames; }
	DWORD GetFreeFrames() const { return m_CapacityFrames - m_FillFrames; }

	// Appends up to numFrames frames (src may be nullptr to append silence).
	// Returns the number of frames actually written (limited by free space).
	DWORD Write(const BYTE* src, DWORD numFrames)
	{
		if (numFrames > GetFreeFrames())
			numFrames = GetFreeFrames();

		DWORD writeFrame = (m_ReadFrame + m_FillFrames) % m_CapacityFrames;
		DWORD remaining = numFrames;
		while (remaining > 0)
		{
			const DWORD chunk = std::min<DWORD>(remaining, m_CapacityFrames - writeFrame);
			BYTE* dst = m_Data.data() + (size_t)writeFrame * m_FrameBytes;
			if (src)
			{
				memcpy(dst, src, (size_t)chunk * m_FrameBytes);
				src += (size_t)chunk * m_FrameBytes;
			}
			else
			{
				memset(dst, 0, (size_t)chunk * m_FrameBytes);
			}
			writeFrame = (writeFrame + chunk) % m_CapacityFrames;
			remaining -= chunk;
		}

		m_FillFrames += numFrames;
		return numFrames;
	}

	// Removes up to numFrames frames into dst (dst may be nullptr to just discard).
	// Returns the number of frames actually read.
	DWORD Read(BYTE* dst, DWORD numFrames)
	{
		if (numFrames > m_FillFrames)
			numFrames = m_FillFrames;

		DWORD remaining = numFrames;
		while (remaining > 0)
		{
			const DWORD chunk = std::min<DWORD>(remaining, m_CapacityFrames - m_ReadFrame);
			if (dst)
			{
				memcpy(dst, m_Data.data() + (size_t)m_ReadFrame * m_FrameBytes, (size_t)chunk * m_FrameBytes);
				dst += (size_t)chunk * m_FrameBytes;
			}
			m_ReadFrame = (m_ReadFrame + chunk) % m_CapacityFrames;
			remaining -= chunk;
		}

		m_FillFrames -= numFrames;
		return numFrames;
	}

private:
	std::vector<BYTE> m_Data;
	DWORD m_FrameBytes = 0;
	DWORD m_CapacityFrames = 0;
	DWORD m_ReadFrame = 0;
	DWORD m_FillFrames = 0;
};
