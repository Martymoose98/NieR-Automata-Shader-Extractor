// Nier Shader Extractor.cpp : Defines the exported functions for the DLL application.
//
#include <Windows.h>
#include <d3dcompiler.h>
#include <time.h>
#include <stdio.h>


#pragma comment(lib, "d3dcompiler.lib")

#define FOURCC(a, b, c, d) ( (DWORD)(((DWORD)((BYTE)a)) | (((DWORD)((BYTE)b)) << 8) | (((DWORD)((BYTE)c)) << 16) | (((DWORD)((BYTE)d)) << 24)) )

#define SHADER_COMPILED_MAGIC FOURCC('D', 'X', 'B', 'C')
#define SHADER_COMPILED_RDEF_CHUNK_MAGIC FOURCC('R', 'D', 'E', 'F')
#define SHADER_COMPILED_ISGN_CHUNK_MAGIC FOURCC('I', 'S', 'G', 'N')

typedef enum _SHADER_TYPE
{
	SHADER_TYPE_COMPUTE = 0x4353,
	SHADER_TYPE_VERTEX = 0xFFFE,
	SHADER_TYPE_PIXEL = 0xFFFF,

} SHADER_TYPE;

//resource definition chunk
typedef struct _SHADER_COMPILED_HEADER
{
	union
	{
		char signature[4];
		DWORD magic;
	};
	BYTE checksum[16];
	DWORD one;
	DWORD dwSize;
	DWORD dwChunkCount;
	DWORD* pdwChunkOffsets;			// not sure if it will aways be five
} SHADER_COMPILED_HEADER, *PSHADER_COMPILED_HEADER;

typedef struct _SHADER_COMPILED_RDEF_CHUNK
{
	union
	{
		char signature[4];
		DWORD magic;
	};
	DWORD dwSize;
	DWORD dwConstantBufferCount;
	DWORD dwConstantBufferDescriptionsOffset;
	DWORD dwResourceBindingCount;
	DWORD dwResourceBindingDescriptionsOffset;
	BYTE MajorVersion;
	BYTE MinorVersion;
	WORD wProgramType;
	DWORD dwCompilerStringOffset;
} SHADER_COMPILED_RDEF_CHUNK, *PSHADER_COMPILED_RDEF_CHUNK;

typedef struct _SHADER_LIST_ENTRY
{
	SHADER_COMPILED_HEADER hdr;
	fpos_t fileoffset;
	const char* szShaderName;
	SHADER_TYPE type;
	void* pShader;
	ID3D10Blob* pDisassembly;
	struct _SCRIPT_LIST_ENTRY* pNext;
} SHADER_LIST_ENTRY, *PSHADER_LIST_ENTRY;

typedef struct DATAFILE_ENUM
{
	const char* szFileName;
	fpos_t rva;
} DATAFILE_ENUM, *PDATAFILE_ENUM;

size_t get_list_size(PSHADER_LIST_ENTRY pHead)
{
	size_t index = 0;

	if (!pHead)
		return 0;

	for (PSHADER_LIST_ENTRY it = pHead; it; it = it->pNext, ++index);

	return index;
}

void free_list(PSHADER_LIST_ENTRY pHead)
{
	PSHADER_LIST_ENTRY pEntry = pHead;
	PSHADER_LIST_ENTRY pNext;

	if (!pEntry)
		return;

	while (pEntry->pNext)
	{
		pNext = pEntry->pNext;

		if (pEntry)
		{
			if (pEntry->pDisassembly)
				pEntry->pDisassembly->lpVtbl->Release(pEntry->pDisassembly);

			if (pEntry->szShaderName)
				free(pEntry->szShaderName);

			if (pEntry->hdr.pdwChunkOffsets)
				free(pEntry->hdr.pdwChunkOffsets);

			if (pEntry->pShader)
				free(pEntry->pShader);

			free(pEntry);
		}
		pEntry = pNext;
	}
}

errno_t open_file(const char* szFilename, FILE** ppFile, size_t* pSize)
{
	if (!szFilename || !ppFile || !pSize)
		return -1;

	errno_t err = fopen_s(ppFile, szFilename, "rb");

	if (err != 0)
		return 1;

	if (fseek(*ppFile, 0, SEEK_END))
		return 2;

	*pSize = ftell(*ppFile);

	if (*pSize == 0xFFFFFFFF)
		return errno;

	rewind(*ppFile);

	return 0;
}

size_t fstr(char** ppszStr, int nByteAdvance, FILE* pStream)
{
	size_t len = 0;
	fpos_t pos;
	fpos_t new_pos;
	char c;

	fgetpos(pStream, &pos);
	new_pos = pos + nByteAdvance;
	fsetpos(pStream, &new_pos);

	do
	{
		fread(&c, sizeof(char), 1, pStream);
		++len;
	} while (c);

	 fsetpos(pStream, &new_pos);

	*ppszStr = malloc(len + 1);

	fprobe(*ppszStr, sizeof(char), len, 0, pStream);
	(*ppszStr)[len] = 0;

	fsetpos(pStream, &pos);

	return len;
}

inline size_t freadrva(void* pBuffer, size_t size, size_t count, int rva, FILE* pStream)
{
	fpos_t pos;
	fpos_t new_pos;
	size_t nElementsRead;

	fgetpos(pStream, &pos);
	new_pos = rva;
	fsetpos(pStream, &new_pos);
	nElementsRead = fread(pBuffer, size, count, pStream);
	fsetpos(pStream, &pos);

	return nElementsRead;
}

inline size_t fprobe(void* pBuffer, size_t size, size_t count, int nBytesAdvance, FILE* pStream)
{
	fpos_t pos;
	fpos_t new_pos;
	size_t nElementsRead;

	fgetpos(pStream, &pos);
	new_pos = pos + nBytesAdvance;
	nElementsRead = fread(pBuffer, size, count, pStream);
	fsetpos(pStream, &new_pos);

	return nElementsRead;
}

inline void fsetposrel(int nBytesAdvance, FILE* pStream)
{
	fpos_t pos;
	fpos_t new_pos;

	fgetpos(pStream, &pos);
	new_pos = pos + nBytesAdvance;
	fsetpos(pStream, &new_pos);
}

inline HRESULT disassemble_shader(PSHADER_LIST_ENTRY pEntry)
{
	return (pEntry) ? D3DDisassemble(pEntry->pShader, pEntry->hdr.dwSize, D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS, NULL, &pEntry->pDisassembly) : E_INVALIDARG;
}

fpos_t get_chunk_offset(FILE* pFile, PSHADER_LIST_ENTRY pEntry, DWORD magic)
{
	DWORD chunk_magic;
	fpos_t offset = -1;
	fpos_t orignal_offset;

	if (pEntry)
	{
		fgetpos(pFile, &orignal_offset);

		for (size_t index = 0; index < pEntry->hdr.dwChunkCount; ++index)
		{
			offset = orignal_offset + pEntry->hdr.pdwChunkOffsets[index];
			fsetpos(pFile, &offset);
			fread(&chunk_magic, sizeof(DWORD), 1, pFile);

			if (chunk_magic == magic)
				break;
			else
				offset = -1;
		}

		fsetpos(pFile, &orignal_offset);
	}
	return offset;
}

PSHADER_LIST_ENTRY parse_shader_binary(FILE* pFile, PDATAFILE_ENUM pEnumerated, PSHADER_LIST_ENTRY pLastEntry)
{
	PSHADER_LIST_ENTRY pEntry = NULL;
	size_t nBytesRead = 0;
	fpos_t offset;
	fpos_t resource_def_chunk_offset;
	SHADER_COMPILED_RDEF_CHUNK rdef_chunk;

	if (!pFile || !pEnumerated)
		return NULL;

	pEntry = malloc(sizeof(SHADER_LIST_ENTRY));

	if (!pEntry)
		return NULL;

	pEntry->pNext = NULL;

	if (pLastEntry)
		pLastEntry->pNext = pEntry;

	pEntry->szShaderName = pEnumerated->szFileName;
	pEntry->fileoffset = pEnumerated->rva;

	fsetpos(pFile, &pEnumerated->rva);

	nBytesRead = fread(&pEntry->hdr, 1, UFIELD_OFFSET(SHADER_COMPILED_HEADER, pdwChunkOffsets), pFile);

	if (nBytesRead < UFIELD_OFFSET(SHADER_COMPILED_HEADER, pdwChunkOffsets))
	{
		printf("Partial Read! Read %x bytes out of %x\n", nBytesRead, pEntry->hdr.dwSize);
	}

	pEntry->hdr.pdwChunkOffsets = malloc(sizeof(DWORD) * pEntry->hdr.dwChunkCount);
	fprobe(pEntry->hdr.pdwChunkOffsets, sizeof(DWORD), pEntry->hdr.dwChunkCount, -FIELD_OFFSET(SHADER_COMPILED_HEADER, pdwChunkOffsets), pFile);

	if (nBytesRead < pEntry->hdr.dwChunkCount)
	{
		printf("Partial Read! Read %x bytes out of %x\n", nBytesRead, pEntry->hdr.dwSize);
	}

	resource_def_chunk_offset = get_chunk_offset(pFile, pEntry, SHADER_COMPILED_RDEF_CHUNK_MAGIC);

	if (resource_def_chunk_offset != -1)
	{
		fgetpos(pFile, &offset);
		fsetpos(pFile, &resource_def_chunk_offset);
		fprobe(&rdef_chunk, 1, sizeof(SHADER_COMPILED_RDEF_CHUNK), 0, pFile);
		pEntry->type = rdef_chunk.wProgramType;
		fsetpos(pFile, &offset);
	}
	else
	{
		printf("[WARNING]: Couldn't determine shader type!\n");
		pEntry->type = 0;
	}

	pEntry->pShader = malloc(pEntry->hdr.dwSize);

	nBytesRead = fread(pEntry->pShader, 1, pEntry->hdr.dwSize, pFile);

	if (nBytesRead < pEntry->hdr.dwSize)
	{
		printf("Partial Read! Read %x bytes out of %x\n", nBytesRead, pEntry->hdr.dwSize);
	}

	if (FAILED(disassemble_shader(pEntry)))
		pEntry->pDisassembly = NULL;

	return pEntry;
}

void data_file_enum_contents(FILE* pFile, struct DATAFILE_ENUM** ppEnum, DWORD* pdwFileCount)
{
	if (!pdwFileCount)
		return;

	DWORD dwOffset;
	DWORD dwNextNameOffset;
	DWORD offset_tbl_rva;
	fpos_t pos;

	fgetpos(pFile, &pos);
	freadrva(pdwFileCount, sizeof(DWORD), 1, 4, pFile);
	freadrva(&offset_tbl_rva, sizeof(DWORD), 1, 8, pFile);
	freadrva(&dwOffset, sizeof(DWORD), 1, 16, pFile);
	freadrva(&dwNextNameOffset, sizeof(DWORD), 1, dwOffset, pFile);

	*ppEnum = calloc(*pdwFileCount, sizeof(struct DATAFILE_ENUM));

	for (DWORD i = 0; i < (*pdwFileCount); ++i)
	{
		fstr(&(*ppEnum)[i].szFileName, dwOffset + 4 + i * dwNextNameOffset, pFile);
		freadrva(&(*ppEnum)[i].rva, sizeof(DWORD), 1, offset_tbl_rva + 4 * i, pFile);
	}
	fsetpos(pFile, &pos);
}

PSHADER_LIST_ENTRY extract_shaders(FILE* pFile, size_t file_size)
{
	DWORD magic;
	PSHADER_LIST_ENTRY pHead = NULL;
	PSHADER_LIST_ENTRY pLast = NULL;
	BOOLEAN bHeadMade = FALSE;
	DWORD file_count;
	PDATAFILE_ENUM pEnumerated;

	if (!pFile)
		return NULL;

	data_file_enum_contents(pFile, &pEnumerated, &file_count);

	for (DWORD i = 0; i < file_count; ++i)
	{
		if (strstr(pEnumerated[i].szFileName, ".pso") || strstr(pEnumerated[i].szFileName, ".vso") || strstr(pEnumerated[i].szFileName, ".cso"))
		{ 
			freadrva(&magic, sizeof(DWORD), 1, pEnumerated[i].rva, pFile);
			if (magic == SHADER_COMPILED_MAGIC)
			{
				if (bHeadMade)
				{
					pLast = parse_shader_binary(pFile, &pEnumerated[i], pLast);
				}
				else
				{
					pHead = parse_shader_binary(pFile, &pEnumerated[i], NULL);
					pLast = pHead;
					bHeadMade = TRUE;
				}
			}
		}
	}

	return pHead;
}

void save_shaders(const char* szSavePath, const char* szPrefix, PSHADER_LIST_ENTRY pHead, BOOLEAN bDumpDissasmbly, BOOLEAN bDumpDecompiled)
{
	char szFilePath[MAX_PATH];
	char szDecompile[MAX_PATH];
	char szCmdline[512];
	char* szShaderFormat;
	char* szShaderDissasemblyFormat;
	char* szTruncate;
	FILE* pFile;
	errno_t err;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	if (bDumpDecompiled)
	{
		GetModuleFileNameA(NULL, szDecompile, MAX_PATH);
		szTruncate = strrchr(szDecompile, '\\');
		strcpy_s(++szTruncate, MAX_PATH - (strlen(szDecompile) + strlen(szTruncate)), "decompile.exe");
	}

	for (PSHADER_LIST_ENTRY it = pHead; it; it = it->pNext)
	{
		if (it->type != SHADER_TYPE_COMPUTE && it->type != SHADER_TYPE_VERTEX && it->type != SHADER_TYPE_PIXEL)
			continue;

		if (szPrefix)
		{
			szShaderFormat = "%s\\%s_%s";
			szShaderDissasemblyFormat = "%s\\disassembly_%s_%s.hlsl";
		}
		else
		{
			szShaderFormat = "%s\\%s";
			szShaderDissasemblyFormat = "%s\\disassembly_%s.hlsl";
		}

		if (szPrefix)
			sprintf_s(szFilePath, MAX_PATH, szShaderFormat, (szSavePath) ? szSavePath : "", szPrefix, it->szShaderName);
		else
			sprintf_s(szFilePath, MAX_PATH, szShaderFormat, (szSavePath) ? szSavePath : "", it->szShaderName);

		err = fopen_s(&pFile, szFilePath, "wb");

		if (!err)
		{
			fwrite(it->pShader, 1, it->hdr.dwSize, pFile);
			fclose(pFile);

			if (bDumpDecompiled)
			{
				memset(&si, 0, sizeof(STARTUPINFOA));
				memset(&pi, 0, sizeof(PROCESS_INFORMATION));
				si.cb = sizeof(STARTUPINFOA);

				sprintf_s(szCmdline, 512, "decompile.exe -D \"%s\"", szFilePath);

				if (!CreateProcessA(szDecompile, szCmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
					printf("CreateProcessA Failed with error code %x\n", GetLastError());

				WaitForSingleObject(pi.hProcess, INFINITE);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
		}

		if (bDumpDissasmbly)
		{
			if (szPrefix)
				sprintf_s(szFilePath, MAX_PATH, szShaderDissasemblyFormat, (szSavePath) ? szSavePath : "", szPrefix, it->szShaderName);
			else
				sprintf_s(szFilePath, MAX_PATH, szShaderDissasemblyFormat, (szSavePath) ? szSavePath : "", it->szShaderName);

			if (it->pDisassembly)
			{
				err = fopen_s(&pFile, szFilePath, "wb");

				if (!err)
				{
					fwrite(it->pDisassembly->lpVtbl->GetBufferPointer(it->pDisassembly), 1, it->pDisassembly->lpVtbl->GetBufferSize(it->pDisassembly), pFile);
					fclose(pFile);
				}
			}
		}
	}
}


void print_welcome()
{
#if WIN32
	printf("Nier Automata Shader Extractor v0.04 by Martino\n\n");
#else
	printf("Nier Automata Shader Extractor (x64) v0.04 by Martino\n\n");
#endif
}

void print_help()
{
	printf("Usage: -d [filename] [dumppath] | -help\n"
		"-d  Dumps all compiled ruby srcipts in the file designated [filename] to the [dumppath].\n"
		"	 [dumppath] is optional and if ommited the scripts will be dumped to the extractor path\n\n"
		"-p  Appends the filename as a prefix to the dumpfile. To be used with -d\n");
}

int main(int argc, char** argv)
{
	FILE* pFile;
	size_t file_size;
	size_t disasm_index;
	char* szFilename = NULL;
	char* szFilenameCopy = NULL;
	char* szDumpPath = NULL;
	char* szRelativeFilename = NULL;
	BOOLEAN bPrefix = FALSE;

	print_welcome();

	if (argc > 1)
	{
		if (!_stricmp(argv[1], "-help"))
		{
			print_help();
			return 1;
		}
		else if (!_stricmp(argv[1], "-d"))
		{
			if (!_stricmp(argv[2], "-p"))
			{
				szFilename = argv[3];

				if (argc >= 5)
					szDumpPath = argv[4];

				bPrefix = TRUE;
			}
			else
			{
				szFilename = argv[2];

				if (argc >= 4)
					szDumpPath = argv[3];
			}
		}
	}
	else
	{
		print_help();
		return 2;
	}

	if (szFilename && bPrefix)
	{
		size_t size = strlen(szFilename) + 1;
		szFilenameCopy = malloc(size);

		if (szFilenameCopy)
		{
			strcpy_s(szFilenameCopy, size, szFilename);
			szRelativeFilename = strrchr(szFilenameCopy, '\\');
			char* szDumpFilePrefix = strstr(szRelativeFilename++, ".");
			*szDumpFilePrefix = 0;
		}
		else
		{
			printf("Could not allocate the required memory!\n");
			return 3;
		}
	}

	if (open_file(szFilename, &pFile, &file_size))
	{
		if (szFilenameCopy)
			free(szFilenameCopy);

		printf("Could not open the file: %s\nPlease check the file path and try again\n", szFilename);
		return 4;
	}

	clock_t start = clock();
	PSHADER_LIST_ENTRY pHead = extract_shaders(pFile, file_size);
	fclose(pFile);

	if (pHead)
	{
		printf("Extracted %d shaders in %.3f seconds\n", get_list_size(pHead), ((float)(clock() - start) / (float)CLOCKS_PER_SEC));
		save_shaders(szDumpPath, szRelativeFilename, pHead, TRUE, TRUE);
		free_list(pHead);
	}

	if (szFilenameCopy)
		free(szFilenameCopy);

	return 0;
}