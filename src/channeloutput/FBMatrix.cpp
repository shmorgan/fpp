/*
 *   FrameBuffer Virtual Matrix handler for Falcon Player (FPP)
 *
 *   Copyright (C) 2013-2018 the Falcon Player Developers
 *      Initial development by:
 *      - David Pitts (dpitts)
 *      - Tony Mace (MyKroFt)
 *      - Mathew Mrosko (Materdaddy)
 *      - Chris Pinkham (CaptainMurdoch)
 *      For additional credits and developers, see credits.php.
 *
 *   The Falcon Player (FPP) is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <linux/kd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "log.h"
#include "FBMatrix.h"
#include "settings.h"

/////////////////////////////////////////////////////////////////////////////
// To disable interpolated scaling on the GPU, add this to /boot/config.txt:
// scaling_kernel=8

/*
 *
 */
FBMatrixOutput::FBMatrixOutput(unsigned int startChannel,
	unsigned int channelCount)
  : ThreadedChannelOutputBase(startChannel, channelCount),
	m_fbFd(0),
	m_ttyFd(0),
	m_width(0),
	m_height(0),
	m_useRGB(0),
	m_inverted(0),
	m_bpp(24),
	m_device("/dev/fb0"),
	m_fbp(nullptr),
	m_screenSize(0),
	m_lastFrame(nullptr),
	m_rgb565map(nullptr)
{
	LogDebug(VB_CHANNELOUT, "FBMatrixOutput::FBMatrixOutput(%u, %u)\n",
		startChannel, channelCount);

	m_useDoubleBuffer = 1;
}

/*
 *
 */
FBMatrixOutput::~FBMatrixOutput()
{
	LogDebug(VB_CHANNELOUT, "FBMatrixOutput::~FBMatrixOutput()\n");

	if (m_lastFrame)
		free(m_lastFrame);

	if (m_rgb565map)
	{
		for (int r = 0; r < 32; r++)
		{
			for (int g = 0; g < 64; g++)
			{
				delete[] m_rgb565map[r][g];
			}

			delete[] m_rgb565map[r];
		}

		delete[] m_rgb565map;
	}
}

/*
 *
 */
int FBMatrixOutput::Init(char *configStr)
{
	LogDebug(VB_CHANNELOUT, "FBMatrixOutput::Init('%s')\n", configStr);

	std::vector<std::string> configElems = split(configStr, ';');

	for (int i = 0; i < configElems.size(); i++)
	{
		std::vector<std::string> elem = split(configElems[i], '=');
		if (elem.size() < 2)
			continue;

		if (elem[0] == "layout")
		{
			m_layout = elem[1];

			std::vector<std::string> parts = split(m_layout, 'x');
			m_width  = atoi(parts[0].c_str());
			m_height = atoi(parts[1].c_str());
		}
		else if (elem[0] == "colorOrder")
		{
			if (elem[1] == "RGB")
				m_useRGB = 1;
		}
		else if (elem[0] == "invert")
		{
			m_inverted = atoi(elem[1].c_str());
		}
		else if (elem[0] == "device")
		{
			m_device = "/dev/";
			m_device += elem[1];
		}
	}

	LogDebug(VB_CHANNELOUT, "Using FrameBuffer device %s\n", m_device.c_str());
	m_fbFd = open(m_device.c_str(), O_RDWR);
	if (!m_fbFd)
	{
		LogErr(VB_CHANNELOUT, "Error opening FrameBuffer device: %s\n", m_device.c_str());
		return 0;
	}

	if (ioctl(m_fbFd, FBIOGET_VSCREENINFO, &m_vInfo))
	{
		LogErr(VB_CHANNELOUT, "Error getting FrameBuffer info\n");
		close(m_fbFd);
		return 0;
	}

	memcpy(&m_vInfoOrig, &m_vInfo, sizeof(struct fb_var_screeninfo));

	if (m_vInfo.bits_per_pixel == 32)
		m_vInfo.bits_per_pixel = 24;

	m_bpp = m_vInfo.bits_per_pixel;
	LogDebug(VB_CHANNELOUT, "FrameBuffer is using %d BPP\n", m_bpp);

	if ((m_bpp != 24) && (m_bpp != 16))
	{
		LogErr(VB_CHANNELOUT, "Do not know how to handle %d BPP\n", m_bpp);
		close(m_fbFd);
		return 0;
	}

	if (m_bpp == 16)
	{
		LogExcess(VB_CHANNELOUT, "Current Bitfield offset info:\n");
		LogExcess(VB_CHANNELOUT, " R: %d (%d bits)\n", m_vInfo.red.offset, m_vInfo.red.length);
		LogExcess(VB_CHANNELOUT, " G: %d (%d bits)\n", m_vInfo.green.offset, m_vInfo.green.length);
		LogExcess(VB_CHANNELOUT, " B: %d (%d bits)\n", m_vInfo.blue.offset, m_vInfo.blue.length);

		// RGB565
		m_vInfo.red.offset    = 11;
		m_vInfo.red.length    = 5;
		m_vInfo.green.offset  = 5;
		m_vInfo.green.length  = 6;
		m_vInfo.blue.offset   = 0;
		m_vInfo.blue.length   = 5;
		m_vInfo.transp.offset = 0;
		m_vInfo.transp.length = 0;

		LogExcess(VB_CHANNELOUT, "New Bitfield offset info should be:\n");
		LogExcess(VB_CHANNELOUT, " R: %d (%d bits)\n", m_vInfo.red.offset, m_vInfo.red.length);
		LogExcess(VB_CHANNELOUT, " G: %d (%d bits)\n", m_vInfo.green.offset, m_vInfo.green.length);
		LogExcess(VB_CHANNELOUT, " B: %d (%d bits)\n", m_vInfo.blue.offset, m_vInfo.blue.length);
	}

	m_vInfo.xres = m_vInfo.xres_virtual = m_width;
	m_vInfo.yres = m_vInfo.yres_virtual = m_height;

	// Config to set the screen back to when we are done
	// Once we determine how this interacts with omxplayer, this may change
	m_vInfoOrig.bits_per_pixel = 16;
	m_vInfoOrig.xres = m_vInfoOrig.xres_virtual = 640;
	m_vInfoOrig.yres = m_vInfoOrig.yres_virtual = 480;

	if (ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfo))
	{
		LogErr(VB_CHANNELOUT, "Error setting FrameBuffer info\n");
		close(m_fbFd);
		return 0;
	}

	if (ioctl(m_fbFd, FBIOGET_FSCREENINFO, &m_fInfo))
	{
		LogErr(VB_CHANNELOUT, "Error getting fixed FrameBuffer info\n");
		close(m_fbFd);
		return 0;
	}

	m_screenSize = m_vInfo.xres * m_vInfo.yres * m_vInfo.bits_per_pixel / 8;

	if (m_screenSize != (m_width * m_height * m_vInfo.bits_per_pixel / 8))
	{
		LogErr(VB_CHANNELOUT, "Error, screensize incorrect\n");
		ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfoOrig);
		close(m_fbFd);
		return 0;
	}

	if (m_channelCount != (m_width * m_height * 3))
	{
		LogErr(VB_CHANNELOUT, "Error, channel count is incorrect\n");
		ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfoOrig);
		close(m_fbFd);
		return 0;
	}

	if (m_device == "/dev/fb0")
	{
		m_ttyFd = open("/dev/console", O_RDWR);
		if (!m_ttyFd)
		{
			LogErr(VB_CHANNELOUT, "Error, unable to open /dev/console\n");
			ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfoOrig);
			close(m_fbFd);
			return 0;
		}

		// Hide the text console
		ioctl(m_ttyFd, KDSETMODE, KD_GRAPHICS);
	}

	m_fbp = (char*)mmap(0, m_screenSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fbFd, 0);

	if ((char *)m_fbp == (char *)-1)
	{
		LogErr(VB_CHANNELOUT, "Error, unable to map /dev/fb0\n");
		ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfoOrig);
		close(m_fbFd);
		return 0;
	}

	m_lastFrame = (unsigned char*)malloc(m_channelCount);
	if (!m_lastFrame)
	{
		LogErr(VB_CHANNELOUT, "Error, unable to allocate lastFrame buffer\n");
		ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfoOrig);
		close(m_fbFd);
		return 0;
	}

	if (m_bpp == 16)
	{
		LogExcess(VB_CHANNELOUT, "Generating RGB565Map for Bitfield offset info:\n");
		LogExcess(VB_CHANNELOUT, " R: %d (%d bits)\n", m_vInfo.red.offset, m_vInfo.red.length);
		LogExcess(VB_CHANNELOUT, " G: %d (%d bits)\n", m_vInfo.green.offset, m_vInfo.green.length);
		LogExcess(VB_CHANNELOUT, " B: %d (%d bits)\n", m_vInfo.blue.offset, m_vInfo.blue.length);

		unsigned char rMask = (0xFF ^ (0xFF >> m_vInfo.red.length));
		unsigned char gMask = (0xFF ^ (0xFF >> m_vInfo.green.length));
		unsigned char bMask = (0xFF ^ (0xFF >> m_vInfo.blue.length));
		int rShift = m_vInfo.red.offset - (8 + (8-m_vInfo.red.length));
		int gShift = m_vInfo.green.offset - (8 + (8 - m_vInfo.green.length));
		int bShift = m_vInfo.blue.offset - (8 + (8 - m_vInfo.blue.length));;

		//LogDebug(VB_CHANNELOUT, "rM/rS: 0x%02x/%d, gM/gS: 0x%02x/%d, bM/bS: 0x%02x/%d\n", rMask, rShift, gMask, gShift, bMask, bShift);

		uint16_t o;
		m_rgb565map = new uint16_t**[32];

		for (uint16_t b = 0; b < 32; b++)
		{
			m_rgb565map[b] = new uint16_t*[64];
			for (uint16_t g = 0; g < 64; g++)
			{
				m_rgb565map[b][g] = new uint16_t[32];
				for (uint16_t r = 0; r < 32; r++)
				{
					o = 0;

					if (rShift >= 0)
						o |= r >> rShift;
					else
						o |= r << abs(rShift);

					if (gShift >= 0)
						o |= g >> gShift;
					else
						o |= g << abs(gShift);

					if (bShift >= 0)
						o |= b >> bShift;
					else
						o |= b << abs(bShift);

					m_rgb565map[b][g][r] = o;
				}
			}
		}
	}

	return ThreadedChannelOutputBase::Init(configStr);
}

/*
 *
 */
int FBMatrixOutput::Close(void)
{
	LogDebug(VB_CHANNELOUT, "FBMatrixOutput::Close()\n");

	munmap(m_fbp, m_screenSize);

	if (m_device == "/dev/fb0")
	{
		if (ioctl(m_fbFd, FBIOPUT_VSCREENINFO, &m_vInfoOrig))
			LogErr(VB_CHANNELOUT, "Error resetting variable info\n");
	}

	close(m_fbFd);

	if (m_device == "/dev/fb0")
	{
		// Re-enable the text console
		ioctl(m_ttyFd, KDSETMODE, KD_TEXT);
		close(m_ttyFd);
	}

	return ChannelOutputBase::Close();
}

/*
 *
 */
int FBMatrixOutput::RawSendData(unsigned char *channelData)
{
	LogExcess(VB_CHANNELOUT, "FBMatrixOutput::SendData(%p)\n",
		channelData);

	int ostride = m_width * m_bpp / 8;
	int srow = 0;
	int drow = m_inverted ? m_height - 1 : 0;
	unsigned char *s = channelData;
	unsigned char *d;
	unsigned char *l = m_lastFrame;
	unsigned char *sR = channelData;
	unsigned char *sG = channelData + 1;
	unsigned char *sB = channelData + 2;
	int skipped = 0;

	if (m_bpp == 16)
	{
		for (int y = 0; y < m_height; y++)
		{
			d = (unsigned char *)m_fbp + (drow * ostride);
			for (int x = 0; x < m_width; x++)
			{
				if (memcmp(l, sR, 3))
				{
					if (skipped)
					{
						sG += skipped * 3;
						sB += skipped * 3;
						d  += skipped * 2;
					}

					if (m_useRGB) // RGB data to BGR framebuffer
						*((uint16_t*)d) = m_rgb565map[*sR >> 3][*sG >> 2][*sB >> 3];
					else // BGR data to BGR framebuffer
						*((uint16_t*)d) = m_rgb565map[*sB >> 3][*sG >> 2][*sR >> 3];

					sG += 3;
					sB += 3;
					d += 2;
				}
				else
				{
					skipped++;
				}

				sR += 3;
				l += 3;
			}

			srow++;
			drow += m_inverted ? -1 : 1;
		}
	}
	else if (m_useRGB)
	{
		unsigned char *dR;
		unsigned char *dG;
		unsigned char *dB;

		for (int y = 0; y < m_height; y++)
		{
			// RGB data to BGR framebuffer
			dR = (unsigned char *)m_fbp + (drow * ostride) + 2;
			dG = (unsigned char *)m_fbp + (drow * ostride) + 1;
			dB = (unsigned char *)m_fbp + (drow * ostride) + 0;

			for (int x = 0; x < m_width; x++)
			{
				if (memcmp(l, sB, 3))
				{
					*dR = *sR;
					*dG = *sG;
					*dB = *sB;
				}

				sR += 3;
				sG += 3;
				sB += 3;
				dR += 3;
				dG += 3;
				dB += 3;
			}

			srow++;
			drow += m_inverted ? -1 : 1;
		}
	}
	else
	{
		if (m_inverted)
		{
			int istride = m_width * 3;
			unsigned char *src = channelData;
			unsigned char *dst = (unsigned char *)m_fbp + (ostride * (m_height-1));

			for (int y = 0; y < m_height; y++)
			{
				memcpy(dst, src, istride);
				src += istride;
				dst -= ostride;
			}
		}
		else
		{
			memcpy(m_fbp, channelData, m_screenSize);
		}
	}

	memcpy(m_lastFrame, channelData, m_channelCount);

	return m_channelCount;
}

void FBMatrixOutput::GetRequiredChannelRange(int &min, int & max) {
    min = m_startChannel;
    max = min + (m_width * m_height * 3) - 1;
}

/*
 *
 */
void FBMatrixOutput::DumpConfig(void)
{
	LogDebug(VB_CHANNELOUT, "FBMatrixOutput::DumpConfig()\n");
	LogDebug(VB_CHANNELOUT, "    layout : %s\n", m_layout.c_str());
	LogDebug(VB_CHANNELOUT, "    width  : %d\n", m_width);
	LogDebug(VB_CHANNELOUT, "    height : %d\n", m_height);
}

