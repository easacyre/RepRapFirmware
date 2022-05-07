/*
 * MonoLcd.cpp
 *
 *  Created on: 6 May 2022
 *      Author: David
 */

#include "MonoLcd.h"
#include <Hardware/Spi/SharedSpiDevice.h>

#if SUPPORT_12864_LCD

MonoLcd::MonoLcd(PixelNumber nr, PixelNumber nc, const LcdFont * const fnts[], size_t nFonts, SpiMode mode) noexcept
	: Lcd(nr, nc, fnts, nFonts),
	  device(SharedSpiDevice::GetMainSharedSpiDevice(), LcdSpiClockFrequency, mode, NoPin, true)
{
	imageSize = nr * ((nc + 7)/8);
	image = new uint8_t[imageSize];
}

MonoLcd::~MonoLcd()
{
	delete image;
	pinMode(csPin, INPUT_PULLUP);
	pinMode(a0Pin, INPUT_PULLUP);
}

// Initialise. a0Pin is only used by the ST7567.
void MonoLcd::Init(Pin p_csPin, Pin p_a0Pin, bool csPolarity, uint32_t freq, uint8_t p_contrastRatio, uint8_t p_resistorRatio) noexcept
{
	// All this is SPI-display specific hardware initialisation, which prohibits I2C-display or UART-display support.
	// NOTE: https://github.com/SchmartMaker/RepRapFirmware/tree/ST7565/src/Display did contain this abstraction
	csPin = p_csPin;
	a0Pin = p_a0Pin;
	contrastRatio = p_contrastRatio;
	resistorRatio = p_resistorRatio;
	device.SetClockFrequency(freq);
	device.SetCsPin(csPin);
	device.SetCsPolarity(csPolarity);		// normally active high chip select for ST7920, active low for ST7567
	pinMode(csPin, (csPolarity) ? OUTPUT_LOW : OUTPUT_HIGH);
#ifdef __LPC17xx__
    device.sspChannel = LcdSpiChannel;
#endif

	startRow = numRows;
	startCol = numCols;
	endRow = endCol = nextFlushRow = 0;

	HardwareInit();
}

// Clear a rectangular block of pixels starting at rows, scol ending just before erow, ecol
void MonoLcd::Clear(PixelNumber sRow, PixelNumber sCol, PixelNumber eRow, PixelNumber eCol) noexcept
{
	if (eCol > numCols) { eCol = numCols; }
	if (eRow > numRows) { eRow = numRows; }
	if (sCol < eCol && sRow < eRow)
	{
		uint8_t sMask = ~(0xFF >> (sCol & 7));		// mask of bits we want to keep in the first byte of each row that we modify
		const uint8_t eMask = 0xFF >> (eCol & 7);	// mask of bits we want to keep in the last byte of each row that we modify
		if ((sCol & ~7) == (eCol & ~7))
		{
			sMask |= eMask;							// special case of just clearing some middle bits
		}
		for (PixelNumber r = sRow; r < eRow; ++r)
		{
			uint8_t * p = image + ((r * (numCols/8)) + (sCol/8));
			uint8_t * const endp = image + ((r * (numCols/8)) + (eCol/8));
			*p &= sMask;
			if (p != endp)
			{
				while (++p < endp)
				{
					*p = 0;
				}
				if ((eCol & 7) != 0)
				{
					*p &= eMask;
				}
			}
		}

		// Flag cleared part as dirty
		if (sCol < startCol) { startCol = sCol; }
		if (eCol >= endCol) { endCol = eCol; }
		if (sRow < startRow) { startRow = sRow; }
		if (eRow >= endRow) { endRow = eRow; }

		SetCursor(sRow, sCol);
		textInverted = false;
		leftMargin = sCol;
		rightMargin = eCol;
	}
}

// Flag a rectangle as dirty. Inline because it is called from only two places.
inline void MonoLcd::SetRectDirty(PixelNumber top, PixelNumber left, PixelNumber bottom, PixelNumber right) noexcept
{
	if (top < startRow) startRow = top;
	if (bottom > endRow) endRow = bottom;
	if (left < startCol) startCol = left;
	if (right > endCol) endCol = right;
}

// Flag a pixel as dirty. The r and c parameters must be no greater than NumRows-1 and NumCols-1 respectively.
void MonoLcd::SetDirty(PixelNumber r, PixelNumber c) noexcept
{
	SetRectDirty(r, c, r + 1, c + 1);
}

// Write one column of character data at (row, column)
void MonoLcd::WriteColumnData(uint16_t columnData, uint8_t ySize) noexcept
{
	const uint8_t mask1 = 0x80 >> (column & 7);
	const uint8_t mask2 = ~mask1;
	uint8_t *p = image + ((row * (numCols/8)) + (column/8));
	const uint16_t setPixelVal = (textInverted) ? 0 : 1;
	for (uint8_t i = 0; i < ySize; ++i)
	{
		const uint8_t oldVal = *p;
		const uint8_t newVal = ((columnData & 1u) == setPixelVal) ? oldVal | mask1 : oldVal & mask2;
		if (newVal != oldVal)
		{
			*p = newVal;
			SetDirty(row + i, column);
		}
		columnData >>= 1;
		p += (numCols/8);
	}
}

void MonoLcd::SetPixel(PixelNumber y, PixelNumber x, bool mode) noexcept
{
	if (y < numRows && x < rightMargin)
	{
		uint8_t * const p = image + ((y * (numCols/8)) + (x/8));
		const uint8_t mask = 0x80u >> (x%8);
		const uint8_t oldVal = *p;
		uint8_t newVal;
		if (mode)
		{
			newVal = oldVal | mask;
		}
		else
		{
			newVal = oldVal & ~mask;
		}

		if (newVal != oldVal)
		{
			*p = newVal;
			SetDirty(y, x);
		}
	}
}

// Draw a bitmap. x0 and numCols must be divisible by 8.
void MonoLcd::BitmapImage(PixelNumber x0, PixelNumber y0, PixelNumber width, PixelNumber height, const uint8_t data[]) noexcept
{
	for (PixelNumber r = 0; r < height && r + y0 < numRows; ++r)
	{
		uint8_t *p = image + (((r + y0) * (numCols/8)) + (x0/8));
		uint16_t bitMapOffset = r * (width/8);
		for (PixelNumber c = 0; c < (width/8) && c + (x0/8) < numCols/8; ++c)
		{
			*p++ = data[bitMapOffset++];
		}
	}

	// Assume the whole area has changed
	if (x0 < startCol) startCol = x0;
	if (x0 + width > endCol) endCol = x0 + width;
	if (y0 < startRow) startRow = y0;
	if (y0 + height > endRow) endRow = y0 + height;
}

// Draw a single bitmap row. 'left' and 'width' do not need to be divisible by 8.
void MonoLcd::BitmapRow(PixelNumber top, PixelNumber left, PixelNumber width, const uint8_t data[], bool invert) noexcept
{
	if (width != 0 && top < numRows)									// avoid possible arithmetic underflow or overflowing the buffer
	{
		const uint8_t inv = (invert) ? 0xFF : 0;
		uint8_t firstColIndex = left/8;									// column index of the first byte to write
		const uint8_t lastColIndex = (left + width - 1)/8;				// column index of the last byte to write
		const unsigned int firstDataShift = left % 8;					// number of bits in the first byte that we leave alone
		uint8_t *p = image + (top * numCols/8) + firstColIndex;

		// Do all bytes except the last one
		uint8_t accumulator = *p & (0xFF << (8 - firstDataShift));		// prime the accumulator
		while (firstColIndex < lastColIndex)
		{
			const uint8_t invData = *data ^ inv;
			const uint8_t newVal = accumulator | (invData >> firstDataShift);
			if (newVal != *p)
			{
				*p = newVal;
				SetDirty(top, 8 * firstColIndex);
			}
			accumulator = invData << (8 - firstDataShift);
			++p;
			++data;
			++firstColIndex;
		}

		// Do the last byte. 'accumulator' contains up to 'firstDataShift' of the most significant bits.
		const unsigned int lastDataShift = 7 - ((left + width - 1) % 8);	// number of trailing bits in the last byte that we leave alone, 0 to 7
		const uint8_t lastMask = (1u << lastDataShift) - 1;					// mask for bits we want to keep;
		accumulator |= (*data ^ inv) >> firstDataShift;
		accumulator &= ~lastMask;
		accumulator |= *p & lastMask;
		if (accumulator != *p)
		{
			*p = accumulator;
			SetDirty(top, 8 * firstColIndex);
		}
	}
}

#endif

// End
