/*
  Hatari - msa.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  MSA Disk support
*/
const char MSA_fileid[] = "Hatari msa.c : " __DATE__ " " __TIME__;

/*
#include <SDL_endian.h>

#include "main.h"
#include "file.h"
#include "floppy.h"
*/

#include "msa.h"

/*
#include "sysdeps.h"
#include "maccess.h"
*/

#define SAVE_TO_MSA_IMAGES

#include "../utils.h"
#include "../debug.h"

#include <stdlib.h>
#include <string.h>

#define Uint16  WORD
#define NUMBYTESPERSECTOR 512					/* All supported disk images are 512 bytes per sector */

/*
    .MSA FILE FORMAT
  --================------------------------------------------------------------

  For those interested, an MSA file is made up as follows:

  Header:

  Word  ID marker, should be $0E0F
  Word  Sectors per track
  Word  Sides (0 or 1; add 1 to this to get correct number of sides)
  Word  Starting track (0-based)
  Word  Ending track (0-based)

  Individual tracks follow the header in alternating side order, e.g. a double
  sided disk is stored as:

  TRACK 0, SIDE 0
  TRACK 0, SIDE 1
  TRACK 1, SIDE 0
  TRACK 1, SIDE 1
  TRACK 2, SIDE 0
  TRACK 2, SIDE 1

  ...and so on. Track blocks are made up as follows:

  Word  Data length
  Bytes  Data

  If the data length is equal to 512 x the sectors per track value, it is an
  uncompressed track and you can merely copy the data to the appropriate track
  of the disk. However, if the data length value is less than 512 x the sectors
  per track value it is a compressed track.

  Compressed tracks use simple a Run Length Encoding (RLE) compression method.
  You can directly copy any data bytes until you find an $E5 byte. This signals
  a compressed run, and is made up as follows:

  Byte  Marker - $E5
  Byte  Data byte
  Word  Run length

  So, if MSA found six $AA bytes in a row it would encode it as:

  $E5AA0006

  What happens if there's an actual $E5 byte on the disk? Well, logically
  enough, it is encoded as:

  $E5E50001

  This is obviously bad news if a disk consists of lots of data like
  $E500E500E500E500... but if MSA makes a track bigger when attempting to
  compress it, it just stores the uncompressed version instead.

  MSA only compresses runs of at least 4 identical bytes (after all, it would be
  wasteful to store 4 bytes for a run of only 3 identical bytes!). There is one
  exception to this rule: if a run of 2 or 3 $E5 bytes is found, that is stored
  appropriately enough as a run. Again, it would be wasteful to store 4 bytes
  for every single $E5 byte.

  The hacked release of MSA that enables the user to turn off compression
  completely simply stops MSA from trying this compression and produces MSA
  images that are completely uncompressed. This is okay because it is possible
  for MSA to produce such an image anyway, and such images are therefore 100%
  compatible with normal MSA versions (and MSA-to-ST of course).
*/

typedef struct
{
	short int ID;                 /* Word  ID marker, should be $0E0F */
	short int SectorsPerTrack;    /* Word  Sectors per track */
	short int Sides;              /* Word  Sides (0 or 1; add 1 to this to get correct number of sides) */
	short int StartingTrack;      /* Word  Starting track (0-based) */
	short int EndingTrack;        /* Word  Ending track (0-based) */
} MSAHEADERSTRUCT;

#define MSA_WORKSPACE_SIZE  (1024*1024)  /* Size of workspace to use when saving MSA files */


static WORD do_get_mem_word(void *a)
{
	return Utils::SWAPWORD2(*((WORD *)a));
}

/*-----------------------------------------------------------------------*/
/**
 * Does filename end with a .MSA extension? If so, return true
 */
/*
bool MSA_FileNameIsMSA(const char *pszFileName, bool bAllowGZ)
{
	return(File_DoesFileExtensionMatch(pszFileName,".msa")
	       || (bAllowGZ && File_DoesFileExtensionMatch(pszFileName,".msa.gz")));
}
*/

/*-----------------------------------------------------------------------*/
/**
 * Uncompress .MSA data into a new buffer.
 */
Uint8 *MSA_UnCompress(Uint8 *pMSAFile, long *pImageSize)
{
	MSAHEADERSTRUCT *pMSAHeader;
	Uint8 *pMSAImageBuffer, *pImageBuffer;
	Uint8 Byte,Data;
	int i,Track,Side,DataLength,NumBytesUnCompressed,RunLength;
	Uint8 *pBuffer = NULL;

	*pImageSize = 0;

	/* Is an '.msa' file?? Check header */
	pMSAHeader = (MSAHEADERSTRUCT *)pMSAFile;
	if (pMSAHeader->ID == Utils::SWAPWORD2(0x0E0F))
	{
		/* First swap 'header' words around to PC format - easier later on */
		pMSAHeader->SectorsPerTrack = Utils::SWAPWORD2(pMSAHeader->SectorsPerTrack);
		pMSAHeader->Sides           = Utils::SWAPWORD2(pMSAHeader->Sides);
		pMSAHeader->StartingTrack   = Utils::SWAPWORD2(pMSAHeader->StartingTrack);
		pMSAHeader->EndingTrack     = Utils::SWAPWORD2(pMSAHeader->EndingTrack);

		/* Create buffer */
		pBuffer = (BYTE *) malloc(  (pMSAHeader->EndingTrack - pMSAHeader->StartingTrack + 1)
		                            * pMSAHeader->SectorsPerTrack * (pMSAHeader->Sides + 1)
		                            * NUMBYTESPERSECTOR);
		if (!pBuffer)
		{
			Debug::out(LOG_ERROR, "MSA_UnCompress");
			return NULL;
		}

		/* Set pointers */
		pImageBuffer = (Uint8 *)pBuffer;
		pMSAImageBuffer = (Uint8 *)((unsigned long)pMSAFile + sizeof(MSAHEADERSTRUCT));

		/* Uncompress to memory as '.ST' disk image - NOTE: assumes 512 bytes
		 * per sector (use NUMBYTESPERSECTOR define)!!! */
		for (Track = pMSAHeader->StartingTrack; Track <= pMSAHeader->EndingTrack; Track++)
		{
			for (Side = 0; Side < (pMSAHeader->Sides+1); Side++)
			{
				int nBytesPerTrack = NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack;

				/* Uncompress MSA Track, first check if is not compressed */
				DataLength = do_get_mem_word(pMSAImageBuffer);
				pMSAImageBuffer += sizeof(short int);
				if (DataLength == nBytesPerTrack)
				{
					/* No compression on track, simply copy and continue */
					memcpy(pImageBuffer, pMSAImageBuffer, nBytesPerTrack);
					pImageBuffer += nBytesPerTrack;
					pMSAImageBuffer += DataLength;
				}
				else
				{
					/* Uncompress track */
					NumBytesUnCompressed = 0;
					while (NumBytesUnCompressed < nBytesPerTrack)
					{
						Byte = *pMSAImageBuffer++;
						if (Byte != 0xE5)                 /* Compressed header?? */
						{
							*pImageBuffer++ = Byte;       /* No, just copy byte */
							NumBytesUnCompressed++;
						}
						else
						{
							Data = *pMSAImageBuffer++;    /* Byte to copy */
							RunLength = do_get_mem_word(pMSAImageBuffer);  /* For length */
							/* Limit length to size of track, incorrect images may overflow */
							if (RunLength+NumBytesUnCompressed > nBytesPerTrack)
							{
								Debug::out(LOG_ERROR, "MSA_UnCompress: Illegal run length -> corrupted disk image?\n");
								RunLength = nBytesPerTrack - NumBytesUnCompressed;
							}
							pMSAImageBuffer += sizeof(short int);
							for (i = 0; i < RunLength; i++)
								*pImageBuffer++ = Data;   /* Copy byte */
							NumBytesUnCompressed += RunLength;
						}
					}
				}
			}
		}

		/* Set size of loaded image */
		*pImageSize = (unsigned long)pImageBuffer-(unsigned long)pBuffer;
	}

	/* Return pointer to buffer, NULL if failed */
	return(pBuffer);
}


/*-----------------------------------------------------------------------*/
/**
 * Uncompress .MSA file into memory, set number bytes of the disk image and
 * return a pointer to the buffer.
 */
/*
Uint8 *MSA_ReadDisk(const char *pszFileName, long *pImageSize)
{
	Uint8 *pMsaFile;
	Uint8 *pDiskBuffer = NULL;

	*pImageSize = 0;

	// Read in file 
	pMsaFile = File_Read(pszFileName, NULL, NULL);
	if (pMsaFile)
	{
		// Uncompress into disk buffer 
		pDiskBuffer = MSA_UnCompress(pMsaFile, pImageSize);

		// Free MSA file we loaded
		free(pMsaFile);
	}

	// Return pointer to buffer, NULL if failed
	return pDiskBuffer;
}
*/

/*-----------------------------------------------------------------------*/
/**
 * Return number of bytes of the same byte in the passed buffer
 * If we return '0' this means no run (or end of buffer)
 */
/*
static int MSA_FindRunOfBytes(Uint8 *pBuffer, int nBytesInBuffer)
{
	Uint8 ScannedByte;
	int nTotalRun;
	bool bMarker;
	int i;

	// Is this the marker? If so, this is at least a run of one.
	bMarker = (*pBuffer == 0xE5);

	// Do we enough for a run?
	if (nBytesInBuffer < 2)
	{
		if (nBytesInBuffer == 1 && bMarker)
			return 1;
		else
			return 0;
	}

	// OK, scan for run
	nTotalRun = 1;
	ScannedByte = *pBuffer++;

	for (i = 1; i < nBytesInBuffer; i++)
	{
		if (*pBuffer++ == ScannedByte)
			nTotalRun++;
		else
			break;
	}

	// Was this enough of a run to make a difference?
	if (nTotalRun < 4 && !bMarker)
		nTotalRun = 0;                  // Just store uncompressed

	return nTotalRun;
}
*/

/*-----------------------------------------------------------------------*/
/**
 * Save compressed .MSA file from memory buffer. Returns true is all OK
 */
/*
bool MSA_WriteDisk(const char *pszFileName, Uint8 *pBuffer, int ImageSize)
{
	MSAHEADERSTRUCT *pMSAHeader;
	unsigned short int *pMSADataLength;
	Uint8 *pMSAImageBuffer, *pMSABuffer, *pImageBuffer;
	Uint16 nSectorsPerTrack, nSides, nCompressedBytes, nBytesPerTrack;
	bool nRet;
	int nTracks,nBytesToGo,nBytesRun;
	int Track,Side;

	// Allocate workspace for compressed image
	pMSAImageBuffer = (Uint8 *)malloc(MSA_WORKSPACE_SIZE);
	if (!pMSAImageBuffer)
	{
		Debug::out(LOG_ERROR, "MSA_WriteDisk");
		return false;
	}

	// Store header
	pMSAHeader = (MSAHEADERSTRUCT *)pMSAImageBuffer;
	pMSAHeader->ID = Utils::SWAPWORD2(0x0E0F);
	Floppy_FindDiskDetails(pBuffer,ImageSize, &nSectorsPerTrack, &nSides);
	pMSAHeader->SectorsPerTrack = Utils::SWAPWORD2(nSectorsPerTrack);
	pMSAHeader->Sides = Utils::SWAPWORD2(nSides-1);
	pMSAHeader->StartingTrack = Utils::SWAPWORD2(0);
	nTracks = ((ImageSize / NUMBYTESPERSECTOR) / nSectorsPerTrack) / nSides;
	pMSAHeader->EndingTrack = Utils::SWAPWORD2(nTracks-1);

	// Compress image
	pMSABuffer = pMSAImageBuffer + sizeof(MSAHEADERSTRUCT);
	for (Track = 0; Track < nTracks; Track++)
	{
		for (Side = 0; Side < nSides; Side++)
		{
			// Get track data pointer 
			nBytesPerTrack = NUMBYTESPERSECTOR*nSectorsPerTrack;
			pImageBuffer = pBuffer + (nBytesPerTrack*Side) + ((nBytesPerTrack*nSides)*Track);

			// Skip data length (fill in later)
			pMSADataLength = (Uint16 *)pMSABuffer;
			pMSABuffer += sizeof(Uint16);

			// Compress track
			nBytesToGo = nBytesPerTrack;
			nCompressedBytes = 0;
			while (nBytesToGo > 0)
			{
				nBytesRun = MSA_FindRunOfBytes(pImageBuffer,nBytesToGo);
				if (nBytesRun == 0)
				{
					// Just copy byte
					*pMSABuffer++ = *pImageBuffer++;
					nCompressedBytes++;
					nBytesRun = 1;
				}
				else
				{
					// Store run!
					*pMSABuffer++ = 0xE5;               // Marker 
					*pMSABuffer++ = *pImageBuffer;      // Byte, and follow with 16-bit length
					do_put_mem_word(pMSABuffer, nBytesRun);
					pMSABuffer += sizeof(Uint16);
					pImageBuffer += nBytesRun;
					nCompressedBytes += 4;
				}
				nBytesToGo -= nBytesRun;
			}

			// Is compressed track smaller than the original?
			if (nCompressedBytes < nBytesPerTrack)
			{
				// Yes, store size
				do_put_mem_word(pMSADataLength, nCompressedBytes);
			}
			else
			{
				// No, just store uncompressed track
				do_put_mem_word(pMSADataLength, nBytesPerTrack);
				pMSABuffer = ((Uint8 *)pMSADataLength) + 2;
				pImageBuffer = pBuffer + (nBytesPerTrack*Side) + ((nBytesPerTrack*nSides)*Track);
				memcpy(pMSABuffer,pImageBuffer, nBytesPerTrack);
				pMSABuffer += nBytesPerTrack;
			}
		}
	}

	// And save to file!
	nRet = File_Save(pszFileName,pMSAImageBuffer, pMSABuffer-pMSAImageBuffer, false);

	// Free workspace
	free(pMSAImageBuffer);

	return nRet;
}
*/

