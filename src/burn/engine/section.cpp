//-------------------------------------------------------------------------------------------------
// <copyright file="section.cpp" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
//
// <summary>
//    Module: Core
// </summary>
//-------------------------------------------------------------------------------------------------

#include "precomp.h"


// constants

// If these defaults ever change, be sure to update constants in burn\stub\StubSection.cpp as well.
#define BURN_SECTION_NAME ".wixburn"
#define BURN_SECTION_MAGIC 0x00f14300
#define BURN_SECTION_VERSION 0x00000002
#define MANIFEST_CABINET_TOKEN L"0"

// structs
typedef struct _BURN_SECTION_HEADER
{
    DWORD dwMagic;
    DWORD dwVersion;

    GUID guidBundleId;

    DWORD dwStubSize;
    DWORD dwOriginalChecksum;
    DWORD dwOriginalSignatureOffset;
    DWORD dwOriginalSignatureSize;

    DWORD dwFormat;
    DWORD cContainers;
    DWORD rgcbContainers[1];
} BURN_SECTION_HEADER;


extern "C" HRESULT SectionInitialize(
    __in BURN_SECTION* pSection
    )
{
    HRESULT hr = S_OK;
    LPWSTR sczPath = NULL;
    DWORD cbRead = 0;
    LARGE_INTEGER li = { };
    LONGLONG llSize = 0;
    IMAGE_DOS_HEADER dosHeader = { };
    IMAGE_NT_HEADERS ntHeader = { };
    DWORD dwChecksumOffset = 0;
    DWORD dwCertificateTableOffset = 0;
    DWORD dwSignatureOffset = 0;
    DWORD cbSignature = 0;
    IMAGE_SECTION_HEADER sectionHeader = { };
    DWORD dwOriginalChecksumAndSignatureOffset = 0;
    BURN_SECTION_HEADER* pBurnSectionHeader = NULL;

    // Initialize in case of failure.
    pSection->hEngineFile = INVALID_HANDLE_VALUE;

    // Get a handle to the engine and hold on to it in case the file on disk ever gets moved
    // away (aka: deleted).
    hr = PathForCurrentProcess(&sczPath, NULL);
    ExitOnFailure(hr, "Failed to get path to engine process.");

    pSection->hEngineFile = ::CreateFileW(sczPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ExitOnInvalidHandleWithLastError(pSection->hEngineFile, hr, "Failed to open handle to engine process path: %ls", sczPath);

    //
    // First, make sure we have a valid DOS signature.
    //
    if (!::SetFilePointerEx(pSection->hEngineFile, li, NULL, FILE_BEGIN))
    {
        ExitWithLastError(hr, "Failed to seek to start of file.");
    }

    // read DOS header
    if (!::ReadFile(pSection->hEngineFile, &dosHeader, sizeof(IMAGE_DOS_HEADER), &cbRead, NULL))
    {
        ExitWithLastError(hr, "Failed to read DOS header.");
    }
    else if (sizeof(IMAGE_DOS_HEADER) > cbRead || IMAGE_DOS_SIGNATURE != dosHeader.e_magic)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Failed to find valid DOS image header in buffer.");
    }

    //
    // Now, make sure we have a valid NT signature.
    //

    // seek to new header
    li.QuadPart = dosHeader.e_lfanew;
    if (!::SetFilePointerEx(pSection->hEngineFile, li, NULL, FILE_BEGIN))
    {
        ExitWithLastError(hr, "Failed to seek to NT header.");
    }

    // read NT header
    if (!::ReadFile(pSection->hEngineFile, &ntHeader, sizeof(IMAGE_NT_HEADERS) - sizeof(IMAGE_OPTIONAL_HEADER), &cbRead, NULL))
    {
        ExitWithLastError(hr, "Failed to read NT header.");
    }
    else if ((sizeof(IMAGE_NT_HEADERS) - sizeof(IMAGE_OPTIONAL_HEADER)) > cbRead || IMAGE_NT_SIGNATURE != ntHeader.Signature)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Failed to find valid NT image header in buffer.");
    }

    // Get the table offsets.
    dwChecksumOffset = dosHeader.e_lfanew + sizeof(IMAGE_NT_HEADERS) - sizeof(IMAGE_OPTIONAL_HEADER) + (sizeof(DWORD) * 16);
    dwCertificateTableOffset = dosHeader.e_lfanew + sizeof(IMAGE_NT_HEADERS) - (sizeof(IMAGE_DATA_DIRECTORY) * (IMAGE_NUMBEROF_DIRECTORY_ENTRIES - IMAGE_DIRECTORY_ENTRY_SECURITY));

    // Seek into the certificate table to get the signature size.
    li.QuadPart = dwCertificateTableOffset;
    if (!::SetFilePointerEx(pSection->hEngineFile, li, NULL, FILE_BEGIN))
    {
        ExitWithLastError(hr, "Failed to seek to section info.");
    }

    if (!::ReadFile(pSection->hEngineFile, &dwSignatureOffset, sizeof(dwSignatureOffset), &cbRead, NULL))
    {
        ExitWithLastError(hr, "Failed to read signature offset.");
    }

    if (!::ReadFile(pSection->hEngineFile, &cbSignature, sizeof(cbSignature), &cbRead, NULL))
    {
        ExitWithLastError(hr, "Failed to read signature size.");
    }

    //
    // Finally, get into the section table and look for the Burn section info.
    //

    // seek past optional headers
    li.QuadPart = dosHeader.e_lfanew + sizeof(IMAGE_NT_HEADERS) - sizeof(IMAGE_OPTIONAL_HEADER) + ntHeader.FileHeader.SizeOfOptionalHeader;
    if (!::SetFilePointerEx(pSection->hEngineFile, li, NULL, FILE_BEGIN))
    {
        ExitWithLastError(hr, "Failed to seek past optional headers.");
    }

    // read sections one by one until we find our section
    for (DWORD i = 0; ; ++i)
    {
        // read section
        if (!::ReadFile(pSection->hEngineFile, &sectionHeader, sizeof(IMAGE_SECTION_HEADER), &cbRead, NULL))
        {
            ExitWithLastError(hr, "Failed to read image section header, index: %u", i);
        }
        if (sizeof(IMAGE_SECTION_HEADER) > cbRead)
        {
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            ExitOnRootFailure(hr, "Failed to read complete image section header, index: %u", i);
        }

        // compare header name
        C_ASSERT(sizeof(sectionHeader.Name) == sizeof(BURN_SECTION_NAME) - 1);
        if (0 == memcmp(sectionHeader.Name, BURN_SECTION_NAME, sizeof(sectionHeader.Name)))
        {
            break;
        }

        // fail if we hit the end
        if (i + 1 >= ntHeader.FileHeader.NumberOfSections)
        {
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            ExitOnRootFailure(hr, "Failed to find Burn section.");
        }
    }

    //
    // We've arrived at the section info.
    //

    // check size of section
    if (sizeof(BURN_SECTION_HEADER) > sectionHeader.SizeOfRawData)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Failed to read section info, data to short: %u", sectionHeader.SizeOfRawData);
    }

    // allocate buffer for section info
    pBurnSectionHeader = (BURN_SECTION_HEADER*)MemAlloc(sectionHeader.SizeOfRawData, TRUE);
    ExitOnNull(pBurnSectionHeader, hr, E_OUTOFMEMORY, "Failed to allocate buffer for section info.");

    // seek to section info
    li.QuadPart = sectionHeader.PointerToRawData;
    if (!::SetFilePointerEx(pSection->hEngineFile, li, NULL, FILE_BEGIN))
    {
        ExitWithLastError(hr, "Failed to seek to section info.");
    }

    // Note the location of original checksum and signature information in the burn section header.
    dwOriginalChecksumAndSignatureOffset = sectionHeader.PointerToRawData + (reinterpret_cast<LPBYTE>(&pBurnSectionHeader->dwOriginalChecksum) - reinterpret_cast<LPBYTE>(pBurnSectionHeader));

    // read section info
    if (!::ReadFile(pSection->hEngineFile, pBurnSectionHeader, sectionHeader.SizeOfRawData, &cbRead, NULL))
    {
        ExitWithLastError(hr, "Failed to read section info.");
    }
    else if (sectionHeader.SizeOfRawData > cbRead)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Failed to read complete section info.");
    }

    // validate version of section info
    if (BURN_SECTION_VERSION != pBurnSectionHeader->dwVersion)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Failed to read section info, unsupported version: %08x", pBurnSectionHeader->dwVersion);
    }

    hr = FileSizeByHandle(pSection->hEngineFile, &llSize);
    ExitOnFailure(hr, "Failed to get total size of bundle.");

    pSection->cbStub = pBurnSectionHeader->dwStubSize;

    // If there is an original signature use that to determine the engine size.
    if (pBurnSectionHeader->dwOriginalSignatureOffset)
    {
        pSection->cbEngineSize = pBurnSectionHeader->dwOriginalSignatureOffset + pBurnSectionHeader->dwOriginalSignatureSize;
    }
    else if (dwSignatureOffset) // if there is a signature, use it.
    {
        pSection->cbEngineSize = dwSignatureOffset + cbSignature;
    }
    else // just use the stub and UX container as the size of the engine.
    {
        pSection->cbEngineSize = pSection->cbStub + pBurnSectionHeader->rgcbContainers[0];
    }

    pSection->qwBundleSize = static_cast<DWORD64>(llSize);

    pSection->dwChecksumOffset = dwChecksumOffset;
    pSection->dwCertificateTableOffset = dwCertificateTableOffset;
    pSection->dwOriginalChecksumAndSignatureOffset = dwOriginalChecksumAndSignatureOffset;

    pSection->dwOriginalChecksum = pBurnSectionHeader->dwOriginalChecksum;
    pSection->dwOriginalSignatureOffset = pBurnSectionHeader->dwOriginalSignatureOffset;
    pSection->dwOriginalSignatureSize = pBurnSectionHeader->dwOriginalSignatureSize;

    pSection->dwFormat = pBurnSectionHeader->dwFormat;
    pSection->cContainers = pBurnSectionHeader->cContainers;
    pSection->rgcbContainers = (DWORD*)MemAlloc(sizeof(DWORD) * pSection->cContainers, TRUE);
    ExitOnNull(pSection->rgcbContainers, hr, E_OUTOFMEMORY, "Failed to allocate memory for container sizes.");

    memcpy(pSection->rgcbContainers, pBurnSectionHeader->rgcbContainers, sizeof(DWORD) * pSection->cContainers);

LExit:
    ReleaseMem(pBurnSectionHeader);
    ReleaseStr(sczPath);

    return hr;
}

extern "C" void SectionUninitialize(
    __out BURN_SECTION* pSection
    )
{
    ReleaseMem(pSection->rgcbContainers);
    ReleaseFileHandle(pSection->hEngineFile);
    memset(pSection, 0, sizeof(BURN_SECTION));
}

extern "C" HRESULT SectionGetAttachedContainerInfo(
    __in BURN_SECTION* pSection,
    __in DWORD iContainerIndex,
    __in DWORD dwExpectedType,
    __out DWORD64* pqwOffset,
    __out DWORD64* pqwSize,
    __out BOOL* pfPresent
    )
{
    HRESULT hr = S_OK;

    // validate container info
    if (iContainerIndex >= pSection->cContainers)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Failed to find container info, too few elements: %u", pSection->cContainers);
    }
    else if (dwExpectedType != pSection->dwFormat)
    {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        ExitOnRootFailure(hr, "Unexpected container format.");
    }

    // If we are asking for the UX container, find it right after the stub.
    if (0 == iContainerIndex)
    {
        *pqwOffset = pSection->cbStub;
    }
    else // attached containers start after the whole engine.
    {
        *pqwOffset = pSection->cbEngineSize;
        for (DWORD i = 1; i < iContainerIndex; ++i)
        {
            *pqwOffset += pSection->rgcbContainers[i];
        }
    }

    *pqwSize = pSection->rgcbContainers[iContainerIndex];
    *pfPresent = (*pqwOffset + *pqwSize) <= pSection->qwBundleSize;

    AssertSz(*pfPresent || pSection->qwBundleSize <= *pqwOffset, "An attached container should either be present or completely absent from the bundle. Found a case where the attached container is partially present which is wrong.");

LExit:
    return hr;
}
