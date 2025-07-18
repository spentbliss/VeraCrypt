/*
 Legal Notice: Some portions of the source code contained in this file were
 derived from the source code of TrueCrypt 7.1a, which is
 Copyright (c) 2003-2012 TrueCrypt Developers Association and which is
 governed by the TrueCrypt License 3.0, also from the source code of
 Encryption for the Masses 2.02a, which is Copyright (c) 1998-2000 Paul Le Roux
 and which is governed by the 'License Agreement for Encryption for the Masses'
 Modifications and additions to the original source code (contained in this file)
 and all other portions of this file are Copyright (c) 2013-2025 AM Crypto
 and are governed by the Apache License 2.0 the full text of which is
 contained in the file License.txt included in VeraCrypt binary and source
 code distribution packages. */

#include "Tcdefs.h"
#include "Crc.h"
#include "Random.h"
#include "Dlgcode.h"
#include "Crypto\cpu.h"
#include "Crypto\jitterentropy.h"
#include "Crypto\rdrand.h"
#include <Strsafe.h>
#include <bcrypt.h>

static unsigned __int8 buffer[RNG_POOL_SIZE];
static unsigned char *pRandPool = NULL;
static BOOL bRandDidInit = FALSE;
static int nRandIndex = 0, randPoolReadIndex = 0;
static int HashFunction = DEFAULT_HASH_ALGORITHM;
static BOOL bDidSlowPoll = FALSE;
BOOL volatile bFastPollEnabled = TRUE;	/* Used to reduce CPU load when performing benchmarks */
BOOL volatile bRandmixEnabled = TRUE;	/* Used to reduce CPU load when performing benchmarks */
static BOOL RandomPoolEnrichedByUser = FALSE;
static HANDLE PeriodicFastPollThreadHandle = NULL;

/* Macro to add a single byte to the pool */
#define RandaddByte(x) {\
	if (nRandIndex == RNG_POOL_SIZE) nRandIndex = 0;\
	pRandPool[nRandIndex] = (unsigned char) ((unsigned char)x + pRandPool[nRandIndex]); \
	if (nRandIndex % RANDMIX_BYTE_INTERVAL == 0) Randmix();\
	nRandIndex++; \
	}

/* Macro to add four bytes to the pool */
#define RandaddInt32(x) RandAddInt((unsigned __int32)x);

#define RandaddIntPtr(x) RandAddInt64((unsigned __int64)x);

void RandAddInt (unsigned __int32 x)
{
	RandaddByte(x);
	RandaddByte((x >> 8));
	RandaddByte((x >> 16));
	RandaddByte((x >> 24));
}

void RandAddInt64 (unsigned __int64 x)
{
	RandaddByte(x);
	RandaddByte((x >> 8));
	RandaddByte((x >> 16));
	RandaddByte((x >> 24));

	RandaddByte((x >> 32));
	RandaddByte((x >> 40));
	RandaddByte((x >> 48));
	RandaddByte((x >> 56));
}

#include <tlhelp32.h>
#include "Dlgcode.h"

#ifndef SRC_POS
#define SRC_POS (__FUNCTION__ ":" TC_TO_STRING(__LINE__))
#endif

HHOOK hMouse = NULL;		/* Mouse hook for the random number generator */
HHOOK hKeyboard = NULL;		/* Keyboard hook for the random number generator */
DWORD ProcessedMouseEventsCounter = 0;

/* Variables for thread control, the thread is used to gather up info about
   the system in in the background */
CRITICAL_SECTION critRandProt;	/* The critical section */
BOOL volatile bThreadTerminate = FALSE;	/* This variable is shared among thread's so its made volatile */

/* Network library handle for the SlowPoll function */
HANDLE hNetAPI32 = NULL;

// CryptoAPI
DWORD CryptoAPILastError = ERROR_SUCCESS;

typedef DWORD (WINAPI *RtlNtStatusToDosError_t)(NTSTATUS);
RtlNtStatusToDosError_t pRtlNtStatusToDosError = NULL;

/* Init the random number generator, setup the hooks, and start the thread */
int RandinitWithCheck ( int* pAlreadyInitialized)
{
	BOOL bIgnoreHookError = FALSE;
	DWORD dwLastError = ERROR_SUCCESS;
	HMODULE ntdll;
	if (GetMaxPkcs5OutSize() > RNG_POOL_SIZE)
		TC_THROW_FATAL_EXCEPTION;

	if(bRandDidInit)
	{
		if (pAlreadyInitialized)
			*pAlreadyInitialized = 1;
		return 0;
	}

	if (pAlreadyInitialized)
		*pAlreadyInitialized = 0;

	InitializeCriticalSection (&critRandProt);

	bRandDidInit = TRUE;
	CryptoAPILastError = ERROR_SUCCESS;
	ProcessedMouseEventsCounter = 0;

	if (pRandPool == NULL)
	{
		pRandPool = (unsigned char *) _aligned_malloc (RANDOMPOOL_ALLOCSIZE, 16);
		if (pRandPool == NULL)
			goto error;

		bDidSlowPoll = FALSE;
		RandomPoolEnrichedByUser = FALSE;

		memset (pRandPool, 0, RANDOMPOOL_ALLOCSIZE);
		VirtualLock (pRandPool, RANDOMPOOL_ALLOCSIZE);
	}

#ifndef VCSDK_DLL
	bIgnoreHookError = IsThreadInSecureDesktop(GetCurrentThreadId());
#else
	bIgnoreHookError = TRUE;
#endif
	hKeyboard = SetWindowsHookEx (WH_KEYBOARD, (HOOKPROC)&KeyboardProc, NULL, GetCurrentThreadId ());
	if (hKeyboard == 0 && !bIgnoreHookError) handleWin32Error (0, SRC_POS);

	hMouse = SetWindowsHookEx (WH_MOUSE, (HOOKPROC)&MouseProc, NULL, GetCurrentThreadId ());
	if (hMouse == 0 && !bIgnoreHookError)
	{
		handleWin32Error (0, SRC_POS);
		goto error;
	}

	ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) {
		// If ntdll.dll is not found, return a fallback error code
		CryptoAPILastError = ERROR_MOD_NOT_FOUND;
		goto error;
	}
	else
		pRtlNtStatusToDosError = (RtlNtStatusToDosError_t)GetProcAddress(ntdll, "RtlNtStatusToDosError");

	if (!(PeriodicFastPollThreadHandle = (HANDLE) _beginthreadex (NULL, 0, PeriodicFastPollThreadProc, NULL, 0, NULL)))
		goto error;

	return 0;

error:
	dwLastError = GetLastError();
	RandStop (TRUE);
	SetLastError (dwLastError);
	return 1;
}

int Randinit ()
{
	return RandinitWithCheck (NULL);
}

/* Close everything down, including the thread which is closed down by
   setting a flag which eventually causes the thread function to exit */
void RandStop (BOOL freePool)
{
	if (!bRandDidInit && freePool && pRandPool)
		goto freePool;

	if (bRandDidInit == FALSE)
		return;

	EnterCriticalSection (&critRandProt);

	if (hMouse != 0)
		UnhookWindowsHookEx (hMouse);
	if (hKeyboard != 0)
		UnhookWindowsHookEx (hKeyboard);

	bThreadTerminate = TRUE;

	LeaveCriticalSection (&critRandProt);

	if (PeriodicFastPollThreadHandle)
		WaitForSingleObject (PeriodicFastPollThreadHandle, INFINITE);

	if (hNetAPI32 != 0)
	{
		FreeLibrary (hNetAPI32);
		hNetAPI32 = NULL;
	}


	hMouse = NULL;
	hKeyboard = NULL;
	bThreadTerminate = FALSE;
	DeleteCriticalSection (&critRandProt);

	bRandDidInit = FALSE;

freePool:
	if (freePool)
	{
		bDidSlowPoll = FALSE;
		RandomPoolEnrichedByUser = FALSE;

		if (pRandPool != NULL)
		{
			burn (pRandPool, RANDOMPOOL_ALLOCSIZE);
			_aligned_free (pRandPool);
			pRandPool = NULL;
		}
	}
}

BOOL IsRandomNumberGeneratorStarted ()
{
	return bRandDidInit;
}

void RandSetHashFunction (int hash_algo_id)
{
	if (HashIsDeprecated (hash_algo_id))
		hash_algo_id = DEFAULT_HASH_ALGORITHM;

	HashFunction = hash_algo_id;
}

int RandGetHashFunction (void)
{
	return HashFunction;
}

void SetRandomPoolEnrichedByUserStatus (BOOL enriched)
{
	RandomPoolEnrichedByUser = enriched;
}

BOOL IsRandomPoolEnrichedByUser ()
{
	return RandomPoolEnrichedByUser;
}

/* The random pool mixing function */
BOOL Randmix ()
{
	if (bRandmixEnabled)
	{
		unsigned char hashOutputBuffer [MAX_DIGESTSIZE];
        #ifndef WOLFCRYPT_BACKEND		
		WHIRLPOOL_CTX	wctx;
		blake2s_state   bctx;
		blake2b_state   b2ctx;
		STREEBOG_CTX	stctx;
        #endif
		sha512_ctx		sctx;
		sha256_ctx		s256ctx;
		int poolIndex, digestIndex, digestSize;

		switch (HashFunction)
		{
		case SHA512:
			digestSize = SHA512_DIGESTSIZE;
			break;

		case SHA256:
			digestSize = SHA256_DIGESTSIZE;
			break;

        #ifndef WOLFCRYPT_BACKEND	
               case BLAKE2S:
               case ARGON2: // in case of Argon2, we use Blake2s
			digestSize = BLAKE2S_DIGESTSIZE;
			break;
	
		case WHIRLPOOL:
			digestSize = WHIRLPOOL_DIGESTSIZE;
			break;

		case STREEBOG:
			digestSize = STREEBOG_DIGESTSIZE;
			break;
        #endif
		default:
			TC_THROW_FATAL_EXCEPTION;
		}

		if (RNG_POOL_SIZE % digestSize)
			TC_THROW_FATAL_EXCEPTION;

		for (poolIndex = 0; poolIndex < RNG_POOL_SIZE; poolIndex += digestSize)
		{
			/* Compute the message digest of the entire pool using the selected hash function. */
			switch (HashFunction)
			{
			case SHA512:
				sha512_begin (&sctx);
				sha512_hash (pRandPool, RNG_POOL_SIZE, &sctx);
				sha512_end (hashOutputBuffer, &sctx);
				break;

			case SHA256:
				sha256_begin (&s256ctx);
				sha256_hash (pRandPool, RNG_POOL_SIZE, &s256ctx);
				sha256_end (hashOutputBuffer, &s256ctx);
				break;

#ifndef WOLFCRYPT_BACKEND
			case BLAKE2S:
				blake2s_init(&bctx);
				blake2s_update(&bctx, pRandPool, RNG_POOL_SIZE);
				blake2s_final(&bctx, hashOutputBuffer);
				break;

			case WHIRLPOOL:
				WHIRLPOOL_init (&wctx);
				WHIRLPOOL_add (pRandPool, RNG_POOL_SIZE, &wctx);
				WHIRLPOOL_finalize (&wctx, hashOutputBuffer);
				break;

			case STREEBOG:
				STREEBOG_init (&stctx);
				STREEBOG_add (&stctx, pRandPool, RNG_POOL_SIZE);
				STREEBOG_finalize (&stctx, hashOutputBuffer);
				break;

			case ARGON2:
				// For Argon2, we use the underlying Blake2b hash function
				blake2b_init(&b2ctx, BLAKE2B_OUTBYTES);
				blake2b_update(&b2ctx, pRandPool, RNG_POOL_SIZE);
				blake2b_final(&b2ctx, hashOutputBuffer, BLAKE2B_OUTBYTES);
				break;
#endif
			default:
				// Unknown/wrong ID
				TC_THROW_FATAL_EXCEPTION;
			}

			/* XOR the resultant message digest to the pool at the poolIndex position. */
			for (digestIndex = 0; digestIndex < digestSize; digestIndex++)
			{
				pRandPool [poolIndex + digestIndex] ^= hashOutputBuffer [digestIndex];
			}
		}

		/* Prevent leaks */
		burn (hashOutputBuffer, MAX_DIGESTSIZE);
		switch (HashFunction)
		{
		case SHA512:
			burn (&sctx, sizeof(sctx));
			break;

		case SHA256:
			burn (&s256ctx, sizeof(s256ctx));
			break;

        #ifndef WOLFCRYPT_BACKEND
               case BLAKE2S:
               case ARGON2: // in case of Argon2, we use Blake2s
			burn (&bctx, sizeof(bctx));
			break;

		case WHIRLPOOL:
			burn (&wctx, sizeof(wctx));
			break;

		case STREEBOG:
			burn (&stctx, sizeof(sctx));
			break;
        #endif
		default:
			// Unknown/wrong ID
			TC_THROW_FATAL_EXCEPTION;
		}
	}
	return TRUE;
}

/* Add a buffer to the pool */
void RandaddBuf (void *buf, int len)
{
	int i;
	for (i = 0; i < len; i++)
	{
		RandaddByte (((unsigned char *) buf)[i]);
	}
}

BOOL RandpeekBytes (void* hwndDlg, unsigned char *buf, int len, DWORD* mouseCounter)
{
	if (!bRandDidInit)
		return FALSE;

	if (len > RNG_POOL_SIZE)
	{
		Error ("ERR_NOT_ENOUGH_RANDOM_DATA", (HWND) hwndDlg);
		len = RNG_POOL_SIZE;
	}

	EnterCriticalSection (&critRandProt);
	*mouseCounter = ProcessedMouseEventsCounter;
	memcpy (buf, pRandPool, len);
	LeaveCriticalSection (&critRandProt);

	return TRUE;
}


/* Get len random bytes from the pool (max. RNG_POOL_SIZE bytes per a single call) */
BOOL RandgetBytes (void* hwndDlg, unsigned char *buf, int len, BOOL forceSlowPoll)
{
	return RandgetBytesFull (hwndDlg, buf, len, forceSlowPoll, FALSE);
}

/* Get len random bytes from the pool.
 *  If allowAnyLength is FALSE, then len must be less or equal to RNG_POOL_SIZE
 *  If allowAnyLength is TRUE, then len can have any positive value
 */
BOOL RandgetBytesFull ( void* hwndDlg, unsigned char *buf , int len, BOOL forceSlowPoll , BOOL allowAnyLength)
{
	int i, looplen;
	BOOL ret = TRUE;

	if (!bRandDidInit || HashFunction == 0)
		TC_THROW_FATAL_EXCEPTION;

	EnterCriticalSection (&critRandProt);

	if (bDidSlowPoll == FALSE || forceSlowPoll)
	{
		if (!SlowPoll ())
		{
			handleError ((HWND) hwndDlg, ERR_CAPI_INIT_FAILED, SRC_POS);
			ret = FALSE;
		}
		else
			bDidSlowPoll = TRUE;
	}

	if (!FastPoll ())
	{
		handleError ((HWND) hwndDlg, ERR_CAPI_INIT_FAILED, SRC_POS);
		ret = FALSE;
	}

	/* There's never more than RNG_POOL_SIZE worth of randomess */
	if ( (!allowAnyLength) && (len > RNG_POOL_SIZE))
	{
		Error ("ERR_NOT_ENOUGH_RANDOM_DATA", (HWND) hwndDlg);
		len = RNG_POOL_SIZE;
		LeaveCriticalSection (&critRandProt);
		return FALSE;
	}

	while (len > 0)
	{
		if (len > RNG_POOL_SIZE)
		{
			looplen = RNG_POOL_SIZE;
			len -= RNG_POOL_SIZE;
		}
		else
		{
			looplen = len;
			len = 0;
		}

		// this loop number of bytes is copied from pool to output buffer,
		// pool is rehashed, and output buffer is XORed with new data from pool
		for (i = 0; i < looplen; i++)
		{
			buf[i] = pRandPool[randPoolReadIndex++];
			if (randPoolReadIndex == RNG_POOL_SIZE) randPoolReadIndex = 0;
		}

		/* Invert the pool */
		for (i = 0; i < RNG_POOL_SIZE / 4; i++)
		{
			((unsigned __int32 *) pRandPool)[i] = ~((unsigned __int32 *) pRandPool)[i];
		}

		// Mix the pool
		if (!FastPoll ())
			ret = FALSE;

		// XOR the current pool content into the output buffer to prevent pool state leaks
		for (i = 0; i < looplen; i++)
		{
			buf[i] ^= pRandPool[randPoolReadIndex++];
			if (randPoolReadIndex == RNG_POOL_SIZE) randPoolReadIndex = 0;
		}

		// increment the pointer for the next loop
		buf += looplen;
	}

	LeaveCriticalSection (&critRandProt);

	if (!ret)
		TC_THROW_FATAL_EXCEPTION;

	return ret;
}

/* Capture the mouse, and as long as the event is not the same as the last
   two events, add the crc of the event, and the crc of the time difference
   between this event and the last + the current time to the pool.
   The role of CRC-32 is merely to perform diffusion. Note that the output
   of CRC-32 is subsequently processed using a cryptographically secure hash
   algorithm. */
LRESULT CALLBACK MouseProc (int nCode, WPARAM wParam, LPARAM lParam)
{
	static DWORD dwLastTimer;
	static unsigned __int32 lastCrc, lastCrc2;
	static POINT lastPoint;
	MOUSEHOOKSTRUCT *lpMouse = (MOUSEHOOKSTRUCT *) lParam;

	if (nCode < 0)
		return CallNextHookEx (hMouse, nCode, wParam, lParam);
	else
	{
		DWORD dwTimer = GetTickCount ();
		DWORD j = dwLastTimer - dwTimer;
		unsigned __int32 crc = 0L;
		int i;
		POINT pt = lpMouse->pt;

		dwLastTimer = dwTimer;

		for (i = 0; i < sizeof (MOUSEHOOKSTRUCT); i++)
		{
			crc = UPDC32 (((unsigned char *) lpMouse)[i], crc);
		}

		if (crc != lastCrc && crc != lastCrc2)
		{
			unsigned __int32 timeCrc = 0L;

			for (i = 0; i < 4; i++)
			{
				timeCrc = UPDC32 (((unsigned char *) &j)[i], timeCrc);
			}

			for (i = 0; i < 4; i++)
			{
				timeCrc = UPDC32 (((unsigned char *) &dwTimer)[i], timeCrc);
			}

			EnterCriticalSection (&critRandProt);
			/* only count real mouse messages in entropy estimation */
			if (	(nCode == HC_ACTION) && (wParam == WM_MOUSEMOVE)
				&& ((pt.x != lastPoint.x) || (pt.y != lastPoint.y)))
			{
				ProcessedMouseEventsCounter++;
				lastPoint = pt;
			}
			RandaddInt32 ((unsigned __int32) (crc + timeCrc));
			LeaveCriticalSection (&critRandProt);
		}
		lastCrc2 = lastCrc;
		lastCrc = crc;

	}
	return 0;
}

/* Capture the keyboard, as long as the event is not the same as the last two
   events, add the crc of the event to the pool along with the crc of the time
   difference between this event and the last. The role of CRC-32 is merely to
   perform diffusion. Note that the output of CRC-32 is subsequently processed
   using a cryptographically secure hash algorithm.  */
LRESULT CALLBACK KeyboardProc (int nCode, WPARAM wParam, LPARAM lParam)
{
	static int lLastKey, lLastKey2;
	static DWORD dwLastTimer;
	int nKey = (lParam & 0x00ff0000) >> 16;
	int nCapture = 0;

	if (nCode < 0)
		return CallNextHookEx (hMouse, nCode, wParam, lParam);

	if ((lParam & 0x0000ffff) == 1 && !(lParam & 0x20000000) &&
	    (lParam & 0x80000000))
	{
		if (nKey != lLastKey)
			nCapture = 1;	/* Capture this key */
		else if (nKey != lLastKey2)
			nCapture = 1;	/* Allow for one repeat */
	}
	if (nCapture)
	{
		DWORD dwTimer = GetTickCount ();
		DWORD j = dwLastTimer - dwTimer;
		unsigned __int32 timeCrc = 0L;
		int i;

		dwLastTimer = dwTimer;
		lLastKey2 = lLastKey;
		lLastKey = nKey;

		for (i = 0; i < 4; i++)
		{
			timeCrc = UPDC32 (((unsigned char *) &j)[i], timeCrc);
		}

		for (i = 0; i < 4; i++)
		{
			timeCrc = UPDC32 (((unsigned char *) &dwTimer)[i], timeCrc);
		}

		EnterCriticalSection (&critRandProt);
		RandaddInt32 ((unsigned __int32) (GetCrc32((unsigned char*) &lParam, sizeof(lParam)) + timeCrc));
		LeaveCriticalSection (&critRandProt);
	}

	return CallNextHookEx (hMouse, nCode, wParam, lParam);
}

/* This is the thread function which will poll the system for randomness */
static unsigned __stdcall PeriodicFastPollThreadProc (void *dummy)
{
	UNREFERENCED_PARAMETER (dummy);		/* Remove unused parameter warning */

	for (;;)
	{
		EnterCriticalSection (&critRandProt);

		if (bThreadTerminate)
		{
			bThreadTerminate = FALSE;
			LeaveCriticalSection (&critRandProt);
			_endthreadex (0);
		}
		else if (bFastPollEnabled)
		{
			FastPoll ();
		}

		LeaveCriticalSection (&critRandProt);

		Sleep (FASTPOLL_INTERVAL);
	}
}

/* Type definitions for function pointers to call NetAPI32 functions */

typedef
  DWORD (WINAPI * NETSTATISTICSGET) (LPWSTR szServer, LPWSTR szService,
				     DWORD dwLevel, DWORD dwOptions,
				     LPBYTE * lpBuffer);
typedef
  DWORD (WINAPI * NETAPIBUFFERSIZE) (LPVOID lpBuffer, LPDWORD cbBuffer);
typedef
  DWORD (WINAPI * NETAPIBUFFERFREE) (LPVOID lpBuffer);

NETSTATISTICSGET pNetStatisticsGet = NULL;
NETAPIBUFFERSIZE pNetApiBufferSize = NULL;
NETAPIBUFFERFREE pNetApiBufferFree = NULL;


/* This is the slowpoll function which gathers up network/hard drive
   performance data for the random pool */
BOOL SlowPoll (void)
{
	static int isWorkstation = -1;
	static int cbPerfData = 0x10000;
	HANDLE hDevice;
	LPBYTE lpBuffer;
	DWORD dwSize, status;
	LPWSTR lpszLanW, lpszLanS;
	int nDrive;
	NTSTATUS bStatus = 0;

	/* Find out whether this is an NT server or workstation if necessary */
	if (isWorkstation == -1)
	{
		HKEY hKey;

		if (RegOpenKeyEx (HKEY_LOCAL_MACHINE,
		       L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions",
				  0, KEY_READ, &hKey) == ERROR_SUCCESS)
		{
			wchar_t szValue[32];
			dwSize = sizeof (szValue);

			isWorkstation = TRUE;
			status = RegQueryValueEx (hKey, L"ProductType", 0, NULL,
						  (LPBYTE) szValue, &dwSize);

			if (status == ERROR_SUCCESS && _wcsicmp (szValue, L"WinNT"))
				/* Note: There are (at least) three cases for
				   ProductType: WinNT = NT Workstation,
				   ServerNT = NT Server, LanmanNT = NT Server
				   acting as a Domain Controller */
				isWorkstation = FALSE;

			RegCloseKey (hKey);
		}
	}
	/* Initialize the NetAPI32 function pointers if necessary */
	if (hNetAPI32 == NULL)
	{
		/* Obtain a handle to the module containing the Lan Manager
		   functions */
		wchar_t dllPath[MAX_PATH];
		if (GetSystemDirectory (dllPath, MAX_PATH))
		{
			StringCchCatW(dllPath, ARRAYSIZE(dllPath), L"\\NETAPI32.DLL");
		}
		else
			StringCchCopyW(dllPath, ARRAYSIZE(dllPath), L"C:\\Windows\\System32\\NETAPI32.DLL");

		hNetAPI32 = LoadLibrary (dllPath);
		if (hNetAPI32 != NULL)
		{
			/* Now get pointers to the functions */
			pNetStatisticsGet = (NETSTATISTICSGET) GetProcAddress (hNetAPI32,
							"NetStatisticsGet");
			pNetApiBufferSize = (NETAPIBUFFERSIZE) GetProcAddress (hNetAPI32,
							"NetApiBufferSize");
			pNetApiBufferFree = (NETAPIBUFFERFREE) GetProcAddress (hNetAPI32,
							"NetApiBufferFree");

			/* Make sure we got valid pointers for every NetAPI32
			   function */
			if (pNetStatisticsGet == NULL ||
			    pNetApiBufferSize == NULL ||
			    pNetApiBufferFree == NULL)
			{
				/* Free the library reference and reset the
				   static handle */
				FreeLibrary (hNetAPI32);
				hNetAPI32 = NULL;
			}
		}
	}

	/* Get network statistics.  Note: Both NT Workstation and NT Server
	   by default will be running both the workstation and server
	   services.  The heuristic below is probably useful though on the
	   assumption that the majority of the network traffic will be via
	   the appropriate service */
	lpszLanW = (LPWSTR) WIDE ("LanmanWorkstation");
	lpszLanS = (LPWSTR) WIDE ("LanmanServer");
	if (hNetAPI32 &&
	    pNetStatisticsGet (NULL,
			       isWorkstation ? lpszLanW : lpszLanS,
			       0, 0, &lpBuffer) == 0)
	{
		pNetApiBufferSize (lpBuffer, &dwSize);
		RandaddBuf ((unsigned char *) lpBuffer, dwSize);
		pNetApiBufferFree (lpBuffer);
	}

	/* Get disk I/O statistics for all the hard drives */
	for (nDrive = 0;; nDrive++)
	{
		DISK_PERFORMANCE diskPerformance;
		wchar_t szDevice[24];

		/* Check whether we can access this device */
		StringCchPrintfW (szDevice, ARRAYSIZE(szDevice), L"\\\\.\\PhysicalDrive%d", nDrive);
		hDevice = CreateFile (szDevice, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
				      NULL, OPEN_EXISTING, 0, NULL);
		if (hDevice == INVALID_HANDLE_VALUE)
			break;


		/* Note: This only works if you have turned on the disk
		   performance counters with 'diskperf -y'.  These counters
		   are off by default */
		if (DeviceIoControl (hDevice, IOCTL_DISK_PERFORMANCE, NULL, 0,
				&diskPerformance, sizeof (DISK_PERFORMANCE),
				     &dwSize, NULL))
		{
			RandaddBuf ((unsigned char *) &diskPerformance, dwSize);
		}
		CloseHandle (hDevice);
	}


	bStatus = BCryptGenRandom(NULL, buffer, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (NT_SUCCESS(bStatus))
	{
		RandaddBuf (buffer, sizeof (buffer));
	}
	else
	{
		/* return error in case BCryptGenRandom fails */
		CryptoAPILastError = pRtlNtStatusToDosError (bStatus);
		return FALSE;
	}

	/* use JitterEntropy library to get good quality random bytes based on CPU timing jitter */
	if (0 == jent_entropy_init ())
	{
		struct rand_data *ec = jent_entropy_collector_alloc (1, 0);
		if (ec)
		{
			ssize_t rndLen = jent_read_entropy (ec, (char*) buffer, sizeof (buffer));
			if (rndLen > 0)
				RandaddBuf (buffer, (int) rndLen);
			jent_entropy_collector_free (ec);
		}
	}

#ifndef _M_ARM64
	// use RDSEED or RDRAND from CPU as source of entropy if present
	if (	IsCpuRngEnabled() && 
		(	(HasRDSEED() && RDSEED_getBytes (buffer, sizeof (buffer)))
		||	(HasRDRAND() && RDRAND_getBytes (buffer, sizeof (buffer)))
		))
	{
		RandaddBuf (buffer, sizeof (buffer));
	}
#endif

	burn(buffer, sizeof (buffer));
	Randmix();

	return TRUE;
}


/* This is the fastpoll function which gathers up info by calling various api's */
BOOL FastPoll (void)
{
	int nOriginalRandIndex = nRandIndex;
	static BOOL addedFixedItems = FALSE;
	FILETIME creationTime, exitTime, kernelTime, userTime;
	SIZE_T minimumWorkingSetSize, maximumWorkingSetSize;
	LARGE_INTEGER performanceCount;
	MEMORYSTATUSEX memoryStatus;
	HANDLE handle;
	POINT point;
	NTSTATUS bStatus = 0;

	/* Get various basic pieces of system information */
	RandaddIntPtr (GetActiveWindow ());	/* Handle of active window */
	RandaddIntPtr (GetCapture ());	/* Handle of window with mouse
					   capture */
	RandaddIntPtr (GetClipboardOwner ());	/* Handle of clipboard owner */
	RandaddIntPtr (GetClipboardViewer ());	/* Handle of start of
						   clpbd.viewer list */
	RandaddIntPtr (GetCurrentProcess ());	/* Pseudohandle of current
						   process */
	RandaddInt32 (GetCurrentProcessId ());	/* Current process ID */
	RandaddIntPtr (GetCurrentThread ());	/* Pseudohandle of current
						   thread */
	RandaddInt32 (GetCurrentThreadId ());	/* Current thread ID */
	RandaddInt32 (GetCurrentTime ());	/* Milliseconds since Windows
						   started */
	RandaddIntPtr (GetDesktopWindow ());	/* Handle of desktop window */
	RandaddIntPtr (GetFocus ());	/* Handle of window with kb.focus */
	RandaddInt32 (GetInputState ());	/* Whether sys.queue has any events */
	RandaddInt32 (GetMessagePos ());	/* Cursor pos.for last message */
	RandaddInt32 (GetMessageTime ());	/* 1 ms time for last message */
	RandaddIntPtr (GetOpenClipboardWindow ());	/* Handle of window with
							   clpbd.open */
	RandaddIntPtr (GetProcessHeap ());	/* Handle of process heap */
	RandaddIntPtr (GetProcessWindowStation ());	/* Handle of procs
							   window station */
	RandaddInt32 (GetQueueStatus (QS_ALLEVENTS));	/* Types of events in
							   input queue */

	/* Get multiword system information */
	GetCaretPos (&point);	/* Current caret position */
	RandaddBuf ((unsigned char *) &point, sizeof (POINT));
	GetCursorPos (&point);	/* Current mouse cursor position */
	RandaddBuf ((unsigned char *) &point, sizeof (POINT));

	/* Get percent of memory in use, bytes of physical memory, bytes of
	   free physical memory, bytes in paging file, free bytes in paging
	   file, user bytes of address space, and free user bytes */
	memoryStatus.dwLength = sizeof (MEMORYSTATUSEX);
	GlobalMemoryStatusEx (&memoryStatus);
	RandaddBuf ((unsigned char *) &memoryStatus, sizeof (MEMORYSTATUSEX));

	/* Get thread and process creation time, exit time, time in kernel
	   mode, and time in user mode in 100ns intervals */
	handle = GetCurrentThread ();
	GetThreadTimes (handle, &creationTime, &exitTime, &kernelTime, &userTime);
	RandaddBuf ((unsigned char *) &creationTime, sizeof (FILETIME));
	RandaddBuf ((unsigned char *) &exitTime, sizeof (FILETIME));
	RandaddBuf ((unsigned char *) &kernelTime, sizeof (FILETIME));
	RandaddBuf ((unsigned char *) &userTime, sizeof (FILETIME));
	handle = GetCurrentProcess ();
	GetProcessTimes (handle, &creationTime, &exitTime, &kernelTime, &userTime);
	RandaddBuf ((unsigned char *) &creationTime, sizeof (FILETIME));
	RandaddBuf ((unsigned char *) &exitTime, sizeof (FILETIME));
	RandaddBuf ((unsigned char *) &kernelTime, sizeof (FILETIME));
	RandaddBuf ((unsigned char *) &userTime, sizeof (FILETIME));

	/* Get the minimum and maximum working set size for the current
	   process */
	GetProcessWorkingSetSize (handle, &minimumWorkingSetSize,
				  &maximumWorkingSetSize);
	RandaddIntPtr (minimumWorkingSetSize);
	RandaddIntPtr (maximumWorkingSetSize);

	/* The following are fixed for the lifetime of the process so we only
	   add them once */
	if (addedFixedItems == 0)
	{
		STARTUPINFO startupInfo;

		/* Get name of desktop, console window title, new window
		   position and size, window flags, and handles for stdin,
		   stdout, and stderr */
		startupInfo.cb = sizeof (STARTUPINFO);
		GetStartupInfo (&startupInfo);
		RandaddBuf ((unsigned char *) &startupInfo, sizeof (STARTUPINFO));
		addedFixedItems = TRUE;
	}
	/* The docs say QPC can fail if appropriate hardware is not
	   available. It works on 486 & Pentium boxes, but hasn't been tested
	   for 386 or RISC boxes */
	if (QueryPerformanceCounter (&performanceCount))
		RandaddBuf ((unsigned char *) &performanceCount, sizeof (LARGE_INTEGER));
	else
	{
		/* Millisecond accuracy at best... */
		DWORD dwTicks = GetTickCount ();
		RandaddBuf ((unsigned char *) &dwTicks, sizeof (dwTicks));
	}


	bStatus = BCryptGenRandom(NULL, buffer, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (NT_SUCCESS(bStatus))
	{
		RandaddBuf (buffer, sizeof (buffer));
	}
	else
	{
		/* return error in case BCryptGenRandom fails */
		CryptoAPILastError = pRtlNtStatusToDosError (bStatus);
		return FALSE;
	}

#ifndef _M_ARM64
	// use RDSEED or RDRAND from CPU as source of entropy if enabled
	if (	IsCpuRngEnabled() && 
		(	(HasRDSEED() && RDSEED_getBytes (buffer, sizeof (buffer)))
		||	(HasRDRAND() && RDRAND_getBytes (buffer, sizeof (buffer)))
		))
	{
		RandaddBuf (buffer, sizeof (buffer));
	}
#endif

	burn (buffer, sizeof(buffer));

	/* Apply the pool mixing function */
	Randmix();

	/* Restore the original pool cursor position. If this wasn't done, mouse coordinates
	   could be written to a limited area of the pool, especially when moving the mouse
	   uninterruptedly. The severity of the problem would depend on the length of data
	   written by FastPoll (if it was equal to the size of the pool, mouse coordinates
	   would be written only to a particular 4-byte area, whenever moving the mouse
	   uninterruptedly). */
	nRandIndex = nOriginalRandIndex;

	return TRUE;
}

