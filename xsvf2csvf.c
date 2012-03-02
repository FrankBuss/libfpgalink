/* 
 * Copyright (C) 2011 Chris McClelland
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <makestuff.h>
#include <libfpgalink.h>
#include <libbuffer.h>
#include "xsvf.h"

#define bitsToBytes(x) ((x>>3) + (x&7 ? 1 : 0))
#define CHECK_BUF_STATUS(x) if ( status != BUF_SUCCESS ) { returnCode = x; goto cleanup; }
#define CHECK_RETURN() if ( returnCode ) { goto cleanup; }
#define FAIL(x) returnCode = x; goto cleanup

#define ENABLE_SWAP
#define BUF_SIZE 128

// Global buffer and offset used to implement the iterator
//
typedef struct {
	struct Buffer xsvfBuf;
	uint32 offset;
} XC;

// The buffer iterator. TODO: refactor to return error code on end of buffer.
//
static uint8 getNextByte(XC *xc) {
	return xc->xsvfBuf.data[xc->offset++];
}

// Read "numBytes" bytes from the stream into a temporary buffer, then write them out in the reverse
// order to the supplied buffer "outBuf". If ENABLE_SWAP is undefined, no swapping is done, so the
// output should be identical to the input.
//
static FLStatus swapBytes(XC *xc, uint32 numBytes, struct Buffer *outBuf, const char **error) {
	FLStatus returnCode = FL_SUCCESS;
	uint8 *ptr;
	BufferStatus status;
	#ifdef ENABLE_SWAP
		status = bufAppendZeros(outBuf, numBytes, NULL, error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		ptr = outBuf->data + outBuf->length - 1;
		while ( numBytes-- ) {
			*ptr-- = getNextByte(xc);
		}
	#else
		uint32 initLength = outBuf->length;
		status = bufAppendZeros(outBuf, numBytes, NULL, error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		ptr = outBuf->data + initLength - 1;
		while ( numBytes-- ) {
			*ptr++ = getNextByte(xc);
		}
	#endif
cleanup:
	return returnCode;
}

static FLStatus sendXSize(struct Buffer *outBuf, uint32 xSize, const char **error) {
	FLStatus returnCode = FL_SUCCESS;
	BufferStatus status;
	union {
		uint32 lword;
		uint8 byte[4];
	} u;
	u.lword = xSize;
	status = bufAppendByte(outBuf, XSDRSIZE, error);
	CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
	#if BYTE_ORDER == 1234
		status = bufAppendByte(outBuf, u.byte[3], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		status = bufAppendByte(outBuf, u.byte[2], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		status = bufAppendByte(outBuf, u.byte[1], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		status = bufAppendByte(outBuf, u.byte[0], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
	#elif BYTE_ORDER == 4321
		status = bufAppendByte(outBuf, u.byte[0], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		status = bufAppendByte(outBuf, u.byte[1], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		status = bufAppendByte(outBuf, u.byte[2], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
		status = bufAppendByte(outBuf, u.byte[3], error);
		CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
	#else
		#error Unsupported BYTE_ORDER
	#endif
cleanup:
	return returnCode;
}

void writeLong(struct Buffer *buf, uint32 offset, uint32 value) {
	union {
		uint32 lword;
		uint8 byte[4];
	} u;
	u.lword = value;
	#if BYTE_ORDER == 1234
		buf->data[offset++] = u.byte[3];
		buf->data[offset++] = u.byte[2];
		buf->data[offset++] = u.byte[1];
		buf->data[offset] = u.byte[0];
	#elif BYTE_ORDER == 4321
		buf->data[offset++] = u.byte[0];
		buf->data[offset++] = u.byte[1];
		buf->data[offset++] = u.byte[2];
		buf->data[offset] = u.byte[3];
	#else
		#error Unsupported BYTE_ORDER
	#endif
}

// Parse the XSVF, reversing the byte-ordering of all the bytestreams.
//
static FLStatus xsvfSwapBytes(XC *xc, struct Buffer *outBuf, uint32 *maxBufSize, const char **error) {
	FLStatus returnCode = FL_SUCCESS;
	uint32 newXSize = 0, curXSize = 0, totXSize = 0, totOffset = 0;
	uint32 numBytes;
	BufferStatus status;
	uint8 thisByte;
	uint32 dummy;
	bool zeroMask = false;

	if ( !maxBufSize ) {
		maxBufSize = &dummy;
	}
	*maxBufSize = 0;
	thisByte = getNextByte(xc);
	while ( thisByte != XCOMPLETE ) {
		switch ( thisByte ) {
		case XTDOMASK:{
			// Swap the XTDOMASK bytes.
			uint32 initLength;
			const uint8 *p;
			const uint8 *end;
			if ( newXSize != curXSize ) {
				curXSize = newXSize;
				sendXSize(outBuf, curXSize, error);
			}
			initLength = outBuf->length;
			numBytes = bitsToBytes(curXSize);
			status = bufAppendByte(outBuf, XTDOMASK, error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, numBytes, outBuf, error);
			CHECK_RETURN();
			p = outBuf->data + initLength + 1;
			end = outBuf->data + outBuf->length;
			while ( *p == 0 && p < end ) p++;
			if ( p == end ) {
				// All zeros so delete the command
				outBuf->length = initLength;
				zeroMask = true;
			} else {
				// Keep the command
				if ( numBytes > *maxBufSize ) {
					*maxBufSize = numBytes;
				}
				zeroMask = false;
			}
			break;
		}

		case XSDRTDO:
			// Swap the tdiValue and tdoExpected bytes.
			if ( newXSize != curXSize ) {
				curXSize = newXSize;
				sendXSize(outBuf, curXSize, error);
			}
			numBytes = bitsToBytes(curXSize);
			if ( zeroMask ) {
				// The last mask was all zeros, so replace this XSDRTDO with an XSDR and throw away
				// the tdoExpected bytes.
				status = bufAppendByte(outBuf, XSDR, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				returnCode = swapBytes(xc, numBytes, outBuf, error);
				CHECK_RETURN();
				while ( numBytes-- ) {
					getNextByte(xc);
				}
			} else {
				// The last mask was not all zeros, so we must honour the XSDRTDO's tdoExpected bytes.
				if ( numBytes > BUF_SIZE ) {
					FAIL(FL_UNSUPPORTED_SIZE_ERR);
				}
				if ( numBytes > *maxBufSize ) {
					*maxBufSize = numBytes;
				}
				status = bufAppendByte(outBuf, XSDRTDO, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				returnCode = swapBytes(xc, 2*numBytes, outBuf, error);
				CHECK_RETURN();
			}
			break;

		case XREPEAT:
			// Drop XREPEAT for now. Will probably be needed for CPLDs.
			getNextByte(xc);
			break;
			
		case XRUNTEST:
			// Copy the XRUNTEST bytes as-is.
			status = bufAppendByte(outBuf, XRUNTEST, error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			status = bufAppendByte(outBuf, getNextByte(xc), error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			break;

		case XSIR:
			// Swap the XSIR bytes.
			status = bufAppendByte(outBuf, XSIR, error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			thisByte = getNextByte(xc);
			status = bufAppendByte(outBuf, thisByte, error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, bitsToBytes(thisByte), outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRSIZE:
			// Just store it; if it differs from the old one it will be sent when required
			newXSize = getNextByte(xc);  // Get MSB
			newXSize <<= 8;
			newXSize |= getNextByte(xc);
			newXSize <<= 8;
			newXSize |= getNextByte(xc);
			newXSize <<= 8;
			newXSize |= getNextByte(xc); // Get LSB
			break;

		case XSDRB:
			// Roll XSDRB, XSDRC*, XSDRE into one XSDR
			curXSize = newXSize;
			sendXSize(outBuf, curXSize, error);
			totXSize = curXSize;
			totOffset = outBuf->length - 4;
			status = bufAppendByte(outBuf, XSDR, error);
			CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			returnCode = swapBytes(xc, bitsToBytes(curXSize), outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRC:
			// Just add the XSDRC data to the end of the previous XSDR
			curXSize = newXSize;
			totXSize += curXSize;
			writeLong(outBuf, totOffset, totXSize);
			returnCode = swapBytes(xc, bitsToBytes(curXSize), outBuf, error);
			CHECK_RETURN();
			break;

		case XSDRE:
			// Just add the XSDRE data to the end of the previous XSDR
			curXSize = newXSize;
			totXSize += curXSize;
			writeLong(outBuf, totOffset, totXSize);
			returnCode = swapBytes(xc, bitsToBytes(curXSize), outBuf, error);
			CHECK_RETURN();
			break;

		case XSTATE:
			// There doesn't seem to be much point in these commands, since the other commands have
			// implied state transitions anyway. Just make sure the TAP is initialised to be at
			// Run-Test/Idle before playing the CSVF stream.
			getNextByte(xc);
			break;

		case XENDIR:
			// Only the default XENDIR state (TAPSTATE_RUN_TEST_IDLE) is supported. Fail fast if
			// there's an attempt to switch the XENDIR state to PAUSE_IR.
			thisByte = getNextByte(xc);
			if ( thisByte ) {
				FAIL(FL_UNSUPPORTED_DATA_ERR);
			}
			break;

		case XENDDR:
			// Only the default XENDDR state (TAPSTATE_RUN_TEST_IDLE) is supported. Fail fast if
			// there's an attempt to switch the XENDDR state to PAUSE_DR.
			thisByte = getNextByte(xc);
			if ( thisByte ) {
				FAIL(FL_UNSUPPORTED_DATA_ERR);
			}
			break;

		default:
			// All other commands are unsupported, so fail if they're encountered.
			FAIL(FL_UNSUPPORTED_CMD_ERR);
		}
		thisByte = getNextByte(xc);
	}

	// Add the XCOMPLETE command
	status = bufAppendByte(outBuf, XCOMPLETE, error);
	CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);

	// Uncomment this to dump the result out for debugging purposes...
	//status = bufWriteBinaryFile(outBuf, "foo.xsvf", 0, outBuf->length, error);
	//CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);

	
cleanup:
	return returnCode;
}

static FLStatus compress(const struct Buffer *inBuf, struct Buffer *outBuf, const char **error) {
	FLStatus returnCode = FL_SUCCESS;
	const uint8 *runStart, *runEnd, *bufEnd, *chunkStart, *chunkEnd;
	uint32 runLen, chunkLen;
	BufferStatus status;
	bufEnd = inBuf->data + inBuf->length;
	runStart = chunkStart = inBuf->data;
	status = bufAppendByte(outBuf, 0x00, error);  // Hdr byte: defaults
	CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
	while ( runStart < bufEnd ) {
		// Find next zero
		while ( runStart < bufEnd && *runStart ) {
			runStart++;
		}
		
		// Remember the position of the zero
		runEnd = runStart;

		// Find the end of this run of zeros
		while ( runEnd < bufEnd && !*runEnd ) {
			runEnd++;
		}
		
		// Get the length of this run
		runLen = runEnd - runStart;
		
		// If this run is more than four zeros, break the chunk
		if ( runLen > 8 || runEnd == bufEnd ) {
			chunkEnd = runStart;
			chunkLen = chunkEnd - chunkStart;

			// There is now a chunk starting at chunkStart and ending at chunkEnd (length chunkLen),
			// Followed by a run of zeros starting at runStart and ending at runEnd (length runLen).
			//printf("Chunk: %d bytes followed by %d zeros\n", chunkLen, runLen);
			if ( chunkLen < 256 ) {
				// Short chunk: uint8
				status = bufAppendByte(outBuf, (uint8)chunkLen, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			} else if ( chunkLen < 65536 ) {
				// Medium chunk: uint16 (big-endian)
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((chunkLen>>8)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)(chunkLen&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			} else {
				// Long chunk: uint32 (big-endian)
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((chunkLen>>24)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((chunkLen>>16)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((chunkLen>>8)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)(chunkLen&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			}
			while ( chunkStart < chunkEnd ) {
				status = bufAppendByte(outBuf, *chunkStart++, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			}
			if ( runLen < 256 ) {
				// Short run: uint8
				status = bufAppendByte(outBuf, (uint8)runLen, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			} else if ( runLen < 65536 ) {
				// Medium run: uint16 (big-endian)
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((runLen>>8)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)(runLen&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			} else {
				// Long run: uint32 (big-endian)
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, 0x00, error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((runLen>>24)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((runLen>>16)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)((runLen>>8)&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
				status = bufAppendByte(outBuf, (uint8)(runLen&0x000000FF), error);
				CHECK_BUF_STATUS(FL_BUF_APPEND_ERR);
			}

			chunkStart = runEnd;
		}
		
		// Start the next round from the end of this run
		runStart = runEnd;
	}

cleanup:
	return returnCode;
}

DLLEXPORT(FLStatus) flLoadXsvfAndConvertToCsvf(
	const char *xsvfFile, struct Buffer *csvfBuf, uint32 *maxBufSize,
	struct Buffer *uncompressedBuf, const char **error)
{
	FLStatus returnCode = FL_SUCCESS;
	struct Buffer swapBuf = {0,};
	BufferStatus status;
	XC xc;
	xc.offset = 0;
	if ( !uncompressedBuf ) {
		uncompressedBuf = &swapBuf;
		status = bufInitialise(uncompressedBuf, 0x20000, 0, error);
		CHECK_BUF_STATUS(FL_BUF_INIT_ERR);
	}
	status = bufInitialise(&xc.xsvfBuf, 0x20000, 0, error);
	CHECK_BUF_STATUS(FL_BUF_INIT_ERR);
	status = bufAppendFromBinaryFile(&xc.xsvfBuf, xsvfFile, error);
	CHECK_BUF_STATUS(FL_BUF_LOAD_ERR);
	returnCode = xsvfSwapBytes(&xc,  uncompressedBuf, maxBufSize, error);
	CHECK_RETURN();
	returnCode = compress(uncompressedBuf, csvfBuf, error);
	CHECK_RETURN();
cleanup:
	bufDestroy(&swapBuf);
	bufDestroy(&xc.xsvfBuf);
	return returnCode;
}
