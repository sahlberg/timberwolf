/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Foundation code.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Bas Schouten <bschouten@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
#ifdef MOZ_LOGGING
#define FORCE_PR_LOG /* Allow logging in the release build */
#endif /* MOZ_LOGGING */

#include "gfxDWriteFontList.h"
#include "gfxDWriteFonts.h"
#include "nsUnicharUtils.h"
#include "nsILocaleService.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch2.h"
#include "nsServiceManagerUtils.h"

#include "gfxGDIFontList.h"

#include "nsIWindowsRegKey.h"

using namespace mozilla;

#ifdef PR_LOGGING

#define LOG_FONTLIST(args) PR_LOG(gfxPlatform::GetLog(eGfxLog_fontlist), \
                               PR_LOG_DEBUG, args)
#define LOG_FONTLIST_ENABLED() PR_LOG_TEST( \
                                   gfxPlatform::GetLog(eGfxLog_fontlist), \
                                   PR_LOG_DEBUG)

#define LOG_FONTINIT(args) PR_LOG(gfxPlatform::GetLog(eGfxLog_fontinit), \
                               PR_LOG_DEBUG, args)
#define LOG_FONTINIT_ENABLED() PR_LOG_TEST( \
                                   gfxPlatform::GetLog(eGfxLog_fontinit), \
                                   PR_LOG_DEBUG)

#endif // PR_LOGGING

// font info loader constants

// avoid doing this during startup even on slow machines but try to start
// it soon enough so that system fallback doesn't happen first
static const PRUint32 kDelayBeforeLoadingFonts = 120 * 1000; // 2 minutes after init
static const PRUint32 kIntervalBetweenLoadingFonts = 2000;   // every 2 seconds until complete

static __inline void
BuildKeyNameFromFontName(nsAString &aName)
{
    if (aName.Length() >= LF_FACESIZE)
        aName.Truncate(LF_FACESIZE - 1);
    ToLowerCase(aName);
}

////////////////////////////////////////////////////////////////////////////////
// gfxDWriteFontFamily

gfxDWriteFontFamily::~gfxDWriteFontFamily()
{
}

void
gfxDWriteFontFamily::FindStyleVariations()
{
    HRESULT hr;
    if (mHasStyles) {
        return;
    }
    mHasStyles = PR_TRUE;

    for (UINT32 i = 0; i < mDWFamily->GetFontCount(); i++) {
        nsRefPtr<IDWriteFont> font;
        hr = mDWFamily->GetFont(i, getter_AddRefs(font));
        if (FAILED(hr)) {
            // This should never happen.
            NS_WARNING("Failed to get existing font from family.");
            continue;
        }

        if (font->GetSimulations() & DWRITE_FONT_SIMULATIONS_OBLIQUE) {
            // We don't want these.
            continue;
        }

        nsRefPtr<IDWriteLocalizedStrings> names;
        hr = font->GetFaceNames(getter_AddRefs(names));
        if (FAILED(hr)) {
            continue;
        }
        
        BOOL exists;
        nsAutoTArray<WCHAR,32> faceName;
        UINT32 englishIdx = 0;
        hr = names->FindLocaleName(L"en-us", &englishIdx, &exists);
        if (FAILED(hr)) {
            continue;
        }

        if (!exists) {
            // No english found, use whatever is first in the list.
            englishIdx = 0;
        }
        UINT32 length;
        hr = names->GetStringLength(englishIdx, &length);
        if (FAILED(hr)) {
            continue;
        }
        if (!faceName.SetLength(length + 1)) {
            // Eeep - running out of memory. Unlikely to end well.
            continue;
        }

        hr = names->GetString(englishIdx, faceName.Elements(), length + 1);
        if (FAILED(hr)) {
            continue;
        }

        nsString fullID(mName);
        fullID.Append(NS_LITERAL_STRING(" "));
        fullID.Append(faceName.Elements());

        /**
         * Faces do not have a localized name so we just put the en-us name in
         * here.
         */
        gfxDWriteFontEntry *fe = 
            new gfxDWriteFontEntry(fullID, font);
        AddFontEntry(fe);

#ifdef PR_LOGGING
        if (LOG_FONTLIST_ENABLED()) {
            LOG_FONTLIST(("(fontlist) added (%s) to family (%s)"
                 " with style: %s weight: %d stretch: %d",
                 NS_ConvertUTF16toUTF8(fe->Name()).get(),
                 NS_ConvertUTF16toUTF8(Name()).get(),
                 (fe->IsItalic()) ? "italic" : "normal",
                 fe->Weight(), fe->Stretch()));
        }
#endif
    }

    if (!mAvailableFonts.Length()) {
        NS_WARNING("Family with no font faces in it.");
    }

    if (mIsBadUnderlineFamily) {
        SetBadUnderlineFonts();
    }
}

void
gfxDWriteFontFamily::LocalizedName(nsAString &aLocalizedName)
{
    aLocalizedName.AssignLiteral("Unknown Font");
    HRESULT hr;
    nsresult rv;
    nsCOMPtr<nsILocaleService> ls = do_GetService(NS_LOCALESERVICE_CONTRACTID,
                                                  &rv);
    nsCOMPtr<nsILocale> locale;
    rv = ls->GetApplicationLocale(getter_AddRefs(locale));
    nsString localeName;
    if (NS_SUCCEEDED(rv)) {
        rv = locale->GetCategory(NS_LITERAL_STRING(NSILOCALE_MESSAGE), 
                                 localeName);
    }
    if (NS_FAILED(rv)) {
        localeName.AssignLiteral("en-us");
    }

    nsRefPtr<IDWriteLocalizedStrings> names;

    hr = mDWFamily->GetFamilyNames(getter_AddRefs(names));
    if (FAILED(hr)) {
        return;
    }
    UINT32 idx = 0;
    BOOL exists;
    hr = names->FindLocaleName(localeName.BeginReading(),
                               &idx,
                               &exists);
    if (FAILED(hr)) {
        return;
    }
    if (!exists) {
        // Use english is localized is not found.
        hr = names->FindLocaleName(L"en-us", &idx, &exists);
        if (FAILED(hr)) {
            return;
        }
        if (!exists) {
            // Use 0 index if english is not found.
            idx = 0;
        }
    }
    nsAutoTArray<WCHAR, 32> famName;
    UINT32 length;
    
    hr = names->GetStringLength(idx, &length);
    if (FAILED(hr)) {
        return;
    }
    
    if (!famName.SetLength(length + 1)) {
        // Eeep - running out of memory. Unlikely to end well.
        return;
    }

    hr = names->GetString(idx, famName.Elements(), length + 1);
    if (FAILED(hr)) {
        return;
    }

    aLocalizedName = nsDependentString(famName.Elements());
}

////////////////////////////////////////////////////////////////////////////////
// gfxDWriteFontEntry

gfxDWriteFontEntry::~gfxDWriteFontEntry()
{
}

PRBool
gfxDWriteFontEntry::IsSymbolFont()
{
    if (mFont) {
        return mFont->IsSymbolFont();
    } else {
        return PR_FALSE;
    }
}

static bool
UsingArabicScriptSystemLocale()
{
    LANGID langid = PRIMARYLANGID(::GetSystemDefaultLangID());
    switch (langid) {
    case LANG_ARABIC:
    case LANG_DARI:
    case LANG_PASHTO:
    case LANG_PERSIAN:
    case LANG_SINDHI:
    case LANG_UIGHUR:
    case LANG_URDU:
        return true;
    default:
        return false;
    }
}

nsresult
gfxDWriteFontEntry::GetFontTable(PRUint32 aTableTag,
                                 FallibleTArray<PRUint8> &aBuffer)
{
    gfxDWriteFontList *pFontList = gfxDWriteFontList::PlatformFontList();

    // don't use GDI table loading for symbol fonts or for
    // italic fonts in Arabic-script system locales because of
    // potential cmap discrepancies, see bug 629386
    if (mFont && pFontList->UseGDIFontTableAccess() &&
        !(mItalic && UsingArabicScriptSystemLocale()) &&
        !mFont->IsSymbolFont())
    {
        LOGFONTW logfont = { 0 };
        if (!InitLogFont(mFont, &logfont))
            return NS_ERROR_FAILURE;

        AutoDC dc;
        AutoSelectFont font(dc.GetDC(), &logfont);
        if (font.IsValid()) {
            PRInt32 tableSize =
                ::GetFontData(dc.GetDC(), NS_SWAP32(aTableTag), 0, NULL, NULL);
            if (tableSize != GDI_ERROR) {
                if (aBuffer.SetLength(tableSize)) {
                    ::GetFontData(dc.GetDC(), NS_SWAP32(aTableTag), 0,
                                  aBuffer.Elements(), aBuffer.Length());
                    return NS_OK;
                }
                return NS_ERROR_OUT_OF_MEMORY;
            }
        }
        return NS_ERROR_FAILURE;
    }

    HRESULT hr;
    nsresult rv;
    nsRefPtr<IDWriteFontFace> fontFace;

    rv = CreateFontFace(getter_AddRefs(fontFace));

    if (NS_FAILED(rv)) {
        return rv;
    }

    PRUint8 *tableData;
    PRUint32 len;
    void *tableContext = NULL;
    BOOL exists;
    hr = fontFace->TryGetFontTable(NS_SWAP32(aTableTag),
                                   (const void**)&tableData,
                                   &len,
                                   &tableContext,
                                   &exists);

    if (FAILED(hr) || !exists) {
        return NS_ERROR_FAILURE;
    }
    if (!aBuffer.SetLength(len)) {
        return NS_ERROR_OUT_OF_MEMORY;
    }
    memcpy(aBuffer.Elements(), tableData, len);
    if (tableContext) {
        fontFace->ReleaseFontTable(&tableContext);
    }
    return NS_OK;
}

nsresult
gfxDWriteFontEntry::ReadCMAP()
{
    HRESULT hr;
    nsresult rv;

    // attempt this once, if errors occur leave a blank cmap
    if (mCmapInitialized)
        return NS_OK;
    mCmapInitialized = PR_TRUE;

    // if loading via GDI, just use GetFontTable
    if (mFont && gfxDWriteFontList::PlatformFontList()->UseGDIFontTableAccess()) {
        const PRUint32 kCmapTag = TRUETYPE_TAG('c','m','a','p');
        AutoFallibleTArray<PRUint8,16384> buffer;

        if (GetFontTable(kCmapTag, buffer) != NS_OK)
            return NS_ERROR_FAILURE;
        PRUint8 *cmap = buffer.Elements();

        PRPackedBool  unicodeFont = PR_FALSE, symbolFont = PR_FALSE;
        rv = gfxFontUtils::ReadCMAP(cmap, buffer.Length(),
                                    mCharacterMap, mUVSOffset,
                                    unicodeFont, symbolFont);
        mHasCmapTable = NS_SUCCEEDED(rv);
        return rv;
    }

    // loading using dwrite, don't use GetFontTable to avoid copy
    nsRefPtr<IDWriteFontFace> fontFace;
    rv = CreateFontFace(getter_AddRefs(fontFace));

    if (NS_FAILED(rv)) {
        return rv;
    }

    PRUint8 *tableData;
    PRUint32 len;
    void *tableContext = NULL;
    BOOL exists;
    hr = fontFace->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('c', 'm', 'a', 'p'),
                                   (const void**)&tableData,
                                   &len,
                                   &tableContext,
                                   &exists);
    if (FAILED(hr)) {
        return NS_ERROR_FAILURE;
    }

    PRPackedBool isSymbol = fontFace->IsSymbolFont();
    PRPackedBool isUnicode = PR_TRUE;
    if (exists) {
        rv = gfxFontUtils::ReadCMAP(tableData,
                                    len,
                                    mCharacterMap,
                                    mUVSOffset,
                                    isUnicode,
                                    isSymbol);
    }
    fontFace->ReleaseFontTable(tableContext);

#ifdef PR_LOGGING
    LOG_FONTLIST(("(fontlist-cmap) name: %s, size: %d\n",
                  NS_ConvertUTF16toUTF8(mName).get(), mCharacterMap.GetSize()));
#endif

    mHasCmapTable = NS_SUCCEEDED(rv);
    return rv;
}

gfxFont *
gfxDWriteFontEntry::CreateFontInstance(const gfxFontStyle* aFontStyle,
                                       PRBool aNeedsBold)
{
    return new gfxDWriteFont(this, aFontStyle, aNeedsBold);
}

nsresult
gfxDWriteFontEntry::CreateFontFace(IDWriteFontFace **aFontFace,
                                   DWRITE_FONT_SIMULATIONS aSimulations)
{
    HRESULT hr;
    if (mFont) {
        hr = mFont->CreateFontFace(aFontFace);
    } else if (mFontFile) {
        IDWriteFontFile *fontFile = mFontFile.get();
        hr = gfxWindowsPlatform::GetPlatform()->GetDWriteFactory()->
            CreateFontFace(mFaceType,
                           1,
                           &fontFile,
                           0,
                           aSimulations,
                           aFontFace);
    }
    if (FAILED(hr)) {
        return NS_ERROR_FAILURE;
    }
    return NS_OK;
}

PRBool
gfxDWriteFontEntry::InitLogFont(IDWriteFont *aFont, LOGFONTW *aLogFont)
{
    HRESULT hr;

    BOOL isInSystemCollection;
    IDWriteGdiInterop *gdi = 
        gfxDWriteFontList::PlatformFontList()->GetGDIInterop();
    hr = gdi->ConvertFontToLOGFONT(aFont, aLogFont, &isInSystemCollection);
    return (FAILED(hr) ? PR_FALSE : PR_TRUE);
}

PRBool
gfxDWriteFontEntry::IsCJKFont()
{
    if (mIsCJK != UNINITIALIZED_VALUE) {
        return mIsCJK;
    }

    mIsCJK = PR_FALSE;

    const PRUint32 kOS2Tag = TRUETYPE_TAG('O','S','/','2');
    AutoFallibleTArray<PRUint8,128> buffer;
    if (GetFontTable(kOS2Tag, buffer) != NS_OK) {
        return mIsCJK;
    }

    // ulCodePageRange bit definitions for the CJK codepages,
    // from http://www.microsoft.com/typography/otspec/os2.htm#cpr
    const PRUint32 CJK_CODEPAGE_BITS =
        (1 << 17) | // codepage 932 - JIS/Japan
        (1 << 18) | // codepage 936 - Chinese (simplified)
        (1 << 19) | // codepage 949 - Korean Wansung
        (1 << 20) | // codepage 950 - Chinese (traditional)
        (1 << 21);  // codepage 1361 - Korean Johab

    if (buffer.Length() >= offsetof(OS2Table, sxHeight)) {
        const OS2Table* os2 =
            reinterpret_cast<const OS2Table*>(buffer.Elements());
        if ((PRUint32(os2->codePageRange1) & CJK_CODEPAGE_BITS) != 0) {
            mIsCJK = PR_TRUE;
        }
    }

    return mIsCJK;
}

////////////////////////////////////////////////////////////////////////////////
// gfxDWriteFontList

gfxDWriteFontList::gfxDWriteFontList()
    : mInitialized(PR_FALSE)
{
    mFontSubstitutes.Init();
}

// bug 602792 - CJK systems default to large CJK fonts which cause excessive
//   I/O strain during cold startup due to dwrite caching bugs.  Default to
//   Arial to avoid this.

gfxFontEntry *
gfxDWriteFontList::GetDefaultFont(const gfxFontStyle *aStyle,
                                  PRBool &aNeedsBold)
{
    nsAutoString resolvedName;

    // try Arial first
    if (ResolveFontName(NS_LITERAL_STRING("Arial"), resolvedName)) {
        return FindFontForFamily(resolvedName, aStyle, aNeedsBold);
    }

    // otherwise, use local default
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    BOOL status = ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 
                                          sizeof(ncm), &ncm, 0);
    if (status) {
        if (ResolveFontName(nsDependentString(ncm.lfMessageFont.lfFaceName),
                            resolvedName)) {
            return FindFontForFamily(resolvedName, aStyle, aNeedsBold);
        }
    }

    return nsnull;
}

gfxFontEntry *
gfxDWriteFontList::LookupLocalFont(const gfxProxyFontEntry *aProxyEntry,
                                   const nsAString& aFullname)
{
    PRBool found;
    gfxFontEntry *lookup;

    // initialize name lookup tables if needed
    if (!mFaceNamesInitialized) {
        InitFaceNameLists();
    }

    // lookup in name lookup tables, return null if not found
    if (!(lookup = mPostscriptNames.GetWeak(aFullname, &found)) &&
        !(lookup = mFullnames.GetWeak(aFullname, &found))) 
    {
        return nsnull;
    }
    gfxFontEntry *fe = 
        new gfxDWriteFontEntry(lookup->Name(),
                               static_cast<gfxDWriteFontEntry*>(lookup)->mFont,
                               aProxyEntry->Weight(),
                               aProxyEntry->Stretch(),
                               aProxyEntry->IsItalic());

    return fe;
}

gfxFontEntry *
gfxDWriteFontList::MakePlatformFont(const gfxProxyFontEntry *aProxyEntry,
                                    const PRUint8 *aFontData,
                                    PRUint32 aLength)
{
    nsresult rv;
    nsAutoString uniqueName;
    rv = gfxFontUtils::MakeUniqueUserFontName(uniqueName);
    if (NS_FAILED(rv)) {
        NS_Free((void*)aFontData);
        return nsnull;
    }

    FallibleTArray<PRUint8> newFontData;

    rv = gfxFontUtils::RenameFont(uniqueName, aFontData, aLength, &newFontData);
    NS_Free((void*)aFontData);

    if (NS_FAILED(rv)) {
        return nsnull;
    }
    
    DWORD numFonts = 0;

    nsRefPtr<IDWriteFontFile> fontFile;
    HRESULT hr;

    /**
     * We pass in a pointer to a structure containing a pointer to the array
     * containing the font data and a unique identifier. DWrite will
     * internally copy what is at that pointer, and pass that to
     * CreateStreamFromKey. The array will be empty when the function 
     * succesfully returns since it swaps out the data.
     */
    ffReferenceKey key;
    key.mArray = &newFontData;
    nsCOMPtr<nsIUUIDGenerator> uuidgen =
      do_GetService("@mozilla.org/uuid-generator;1");
    if (!uuidgen) {
        return nsnull;
    }

    rv = uuidgen->GenerateUUIDInPlace(&key.mGUID);

    if (NS_FAILED(rv)) {
        return nsnull;
    }

    hr = gfxWindowsPlatform::GetPlatform()->GetDWriteFactory()->
        CreateCustomFontFileReference(&key,
                                      sizeof(key),
                                      gfxDWriteFontFileLoader::Instance(),
                                      getter_AddRefs(fontFile));

    if (FAILED(hr)) {
        NS_WARNING("Failed to create custom font file reference.");
        return nsnull;
    }

    BOOL isSupported;
    DWRITE_FONT_FILE_TYPE fileType;
    UINT32 numFaces;

    PRUint16 w = (aProxyEntry->mWeight == 0 ? 400 : aProxyEntry->mWeight);
    gfxDWriteFontEntry *entry = 
        new gfxDWriteFontEntry(uniqueName, 
                               fontFile,
                               aProxyEntry->Weight(),
                               aProxyEntry->Stretch(),
                               aProxyEntry->IsItalic());

    fontFile->Analyze(&isSupported, &fileType, &entry->mFaceType, &numFaces);
    if (!isSupported || numFaces > 1) {
        // We don't know how to deal with 0 faces either.
        delete entry;
        return nsnull;
    }

    return entry;
}

#ifdef DEBUG_DWRITE_STARTUP

#define LOGREGISTRY(msg) LogRegistryEvent(msg)

// for use when monitoring process
static void LogRegistryEvent(const wchar_t *msg)
{
    HKEY dummyKey;
    HRESULT hr;
    wchar_t buf[512];

    wsprintfW(buf, L" log %s", msg);
    hr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, buf, 0, KEY_READ, &dummyKey);
    if (SUCCEEDED(hr)) {
        RegCloseKey(dummyKey);
    }
}
#else

#define LOGREGISTRY(msg)

#endif

nsresult
gfxDWriteFontList::InitFontList()
{
    LOGREGISTRY(L"InitFontList start");

    mInitialized = PR_FALSE;

#ifdef PR_LOGGING
    LARGE_INTEGER frequency;        // ticks per second
    LARGE_INTEGER t1, t2, t3;           // ticks
    double elapsedTime, upTime;
    char nowTime[256], nowDate[256];

    if (LOG_FONTINIT_ENABLED()) {    
        GetTimeFormat(LOCALE_INVARIANT, TIME_FORCE24HOURFORMAT, 
                      NULL, NULL, nowTime, 256);
        GetDateFormat(LOCALE_INVARIANT, NULL, NULL, NULL, nowDate, 256);
        upTime = (double) GetTickCount();
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&t1);
    }
#endif

    HRESULT hr;
    gfxFontCache *fc = gfxFontCache::GetCache();
    if (fc) {
        fc->AgeAllGenerations();
    }

    nsCOMPtr<nsIPrefBranch2> pref = do_GetService(NS_PREFSERVICE_CONTRACTID);
    nsresult rv;

    rv = pref->GetBoolPref(
             "gfx.font_rendering.directwrite.use_gdi_table_loading", 
             &mGDIFontTableAccess);
    if (NS_FAILED(rv)) {
        mGDIFontTableAccess = PR_FALSE;
    }

    gfxPlatformFontList::InitFontList();

    mFontSubstitutes.Clear();
    mNonExistingFonts.Clear();

#ifdef PR_LOGGING
    if (LOG_FONTINIT_ENABLED()) {
        QueryPerformanceCounter(&t2);
    }
#endif

    hr = gfxWindowsPlatform::GetPlatform()->GetDWriteFactory()->
        GetGdiInterop(getter_AddRefs(mGDIInterop));
    if (FAILED(hr)) {
        return NS_ERROR_FAILURE;
    }

    LOGREGISTRY(L"InitFontList end");

#ifdef PR_LOGGING
    if (LOG_FONTINIT_ENABLED()) {
        QueryPerformanceCounter(&t3);

        // determine dwrite version
        nsAutoString dwriteVers;
        gfxWindowsPlatform::GetPlatform()->GetDLLVersion(L"dwrite.dll",
                                                         dwriteVers);
        LOG_FONTINIT(("InitFontList\n"));
        LOG_FONTINIT(("Start: %s %s\n", nowDate, nowTime));
        LOG_FONTINIT(("Uptime: %9.3f s\n", upTime/1000));
        LOG_FONTINIT(("dwrite version: %s\n", 
                      NS_ConvertUTF16toUTF8(dwriteVers).get()));
        elapsedTime = (t3.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
        LOG_FONTINIT(("Total time in InitFontList:    %9.3f ms\n", elapsedTime));
        elapsedTime = (t2.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
        LOG_FONTINIT((" --- gfxPlatformFontList init: %9.3f ms\n", elapsedTime));
        elapsedTime = (t3.QuadPart - t2.QuadPart) * 1000.0 / frequency.QuadPart;
        LOG_FONTINIT((" --- GdiInterop object:        %9.3f ms\n", elapsedTime));
    }
#endif

    return NS_OK;
}

nsresult
gfxDWriteFontList::DelayedInitFontList()
{
    LOGREGISTRY(L"DelayedInitFontList start");

#ifdef PR_LOGGING
    LARGE_INTEGER frequency;        // ticks per second
    LARGE_INTEGER t1, t2, t3;           // ticks
    double elapsedTime, upTime;
    char nowTime[256], nowDate[256];

    if (LOG_FONTINIT_ENABLED()) {    
        GetTimeFormat(LOCALE_INVARIANT, TIME_FORCE24HOURFORMAT, 
                      NULL, NULL, nowTime, 256);
        GetDateFormat(LOCALE_INVARIANT, NULL, NULL, NULL, nowDate, 256);
        upTime = (double) GetTickCount();
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&t1);
    }
#endif

    HRESULT hr;

    LOGREGISTRY(L"calling GetSystemFontCollection");
    nsRefPtr<IDWriteFontCollection> systemFonts;
    hr = gfxWindowsPlatform::GetPlatform()->GetDWriteFactory()->
        GetSystemFontCollection(getter_AddRefs(systemFonts));
    NS_ASSERTION(SUCCEEDED(hr), "GetSystemFontCollection failed!");
    LOGREGISTRY(L"GetSystemFontCollection done");

    if (FAILED(hr)) {
        return NS_ERROR_FAILURE;
    }

#ifdef PR_LOGGING
    if (LOG_FONTINIT_ENABLED()) {
        QueryPerformanceCounter(&t2);
    }
#endif

    for (UINT32 i = 0; i < systemFonts->GetFontFamilyCount(); i++) {
        nsRefPtr<IDWriteFontFamily> family;
        systemFonts->GetFontFamily(i, getter_AddRefs(family));

        nsRefPtr<IDWriteLocalizedStrings> names;
        hr = family->GetFamilyNames(getter_AddRefs(names));
        if (FAILED(hr)) {
            continue;
        }

        UINT32 englishIdx = 0;

        BOOL exists;
        hr = names->FindLocaleName(L"en-us", &englishIdx, &exists);
        if (FAILED(hr)) {
            continue;
        }
        if (!exists) {
            // Use 0 index if english is not found.
            englishIdx = 0;
        }

        nsAutoTArray<WCHAR, 32> enName;
        UINT32 length;
        
        hr = names->GetStringLength(englishIdx, &length);
        if (FAILED(hr)) {
            continue;
        }
        
        if (!enName.SetLength(length + 1)) {
            // Eeep - running out of memory. Unlikely to end well.
            continue;
        }

        hr = names->GetString(englishIdx, enName.Elements(), length + 1);
        if (FAILED(hr)) {
            continue;
        }

        nsAutoString name(enName.Elements());
        BuildKeyNameFromFontName(name);

        nsRefPtr<gfxFontFamily> fam;

        if (mFontFamilies.GetWeak(name)) {
            continue;
        }
        
        nsDependentString familyName(enName.Elements());

        fam = new gfxDWriteFontFamily(familyName, family);
        if (!fam) {
            continue;
        }

        if (mBadUnderlineFamilyNames.Contains(name)) {
            fam->SetBadUnderlineFamily();
        }
        mFontFamilies.Put(name, fam);

        // now add other family name localizations, if present
        PRUint32 nameCount = names->GetCount();
        PRUint32 nameIndex;

        for (nameIndex = 0; nameIndex < nameCount; nameIndex++) {
            UINT32 nameLen;
            nsAutoTArray<WCHAR, 32> localizedName;

            // only add other names
            if (nameIndex == englishIdx) {
                continue;
            }
            
            hr = names->GetStringLength(nameIndex, &nameLen);
            if (FAILED(hr)) {
                continue;
            }

            if (!localizedName.SetLength(nameLen + 1)) {
                continue;
            }

            hr = names->GetString(nameIndex, localizedName.Elements(), 
                                  nameLen + 1);
            if (FAILED(hr)) {
                continue;
            }

            nsDependentString locName(localizedName.Elements());

            if (!familyName.Equals(locName)) {
                AddOtherFamilyName(fam, locName);
            }

        }

        // at this point, all family names have been read in
        fam->SetOtherFamilyNamesInitialized();
    }

    mOtherFamilyNamesInitialized = PR_TRUE;
    GetFontSubstitutes();

    // bug 551313 - DirectWrite creates a Gill Sans family out of 
    // poorly named members of the Gill Sans MT family containing
    // only Ultra Bold weights.  This causes big problems for pages
    // using Gill Sans which is usually only available on OSX

    nsAutoString nameGillSans(L"Gill Sans");
    nsAutoString nameGillSansMT(L"Gill Sans MT");
    BuildKeyNameFromFontName(nameGillSans);
    BuildKeyNameFromFontName(nameGillSansMT);

    gfxFontFamily *gillSansFamily = mFontFamilies.GetWeak(nameGillSans);
    gfxFontFamily *gillSansMTFamily = mFontFamilies.GetWeak(nameGillSansMT);

    if (gillSansFamily && gillSansMTFamily) {
        gillSansFamily->FindStyleVariations();
        nsTArray<nsRefPtr<gfxFontEntry> >& faces = gillSansFamily->GetFontList();
        PRUint32 i;

        PRBool allUltraBold = PR_TRUE;
        for (i = 0; i < faces.Length(); i++) {
            // does the face have 'Ultra Bold' in the name?
            if (faces[i]->Name().Find(NS_LITERAL_STRING("Ultra Bold")) == -1) {
                allUltraBold = PR_FALSE;
                break;
            }
        }

        // if all the Gill Sans faces are Ultra Bold ==> move faces
        // for Gill Sans into Gill Sans MT family
        if (allUltraBold) {

            // add faces to Gill Sans MT
            for (i = 0; i < faces.Length(); i++) {
                gillSansMTFamily->AddFontEntry(faces[i]);

#ifdef PR_LOGGING
                if (LOG_FONTLIST_ENABLED()) {
                    gfxFontEntry *fe = faces[i];
                    LOG_FONTLIST(("(fontlist) moved (%s) to family (%s)"
                         " with style: %s weight: %d stretch: %d",
                         NS_ConvertUTF16toUTF8(fe->Name()).get(),
                         NS_ConvertUTF16toUTF8(gillSansMTFamily->Name()).get(),
                         (fe->IsItalic()) ? "italic" : "normal",
                         fe->Weight(), fe->Stretch()));
                }
#endif
            }

            // remove Gills Sans
            mFontFamilies.Remove(nameGillSans);
        }
    }

    StartLoader(kDelayBeforeLoadingFonts, kIntervalBetweenLoadingFonts);

    LOGREGISTRY(L"DelayedInitFontList end");

#ifdef PR_LOGGING
    if (LOG_FONTINIT_ENABLED()) {
        QueryPerformanceCounter(&t3);

        // determine dwrite version
        nsAutoString dwriteVers;
        gfxWindowsPlatform::GetPlatform()->GetDLLVersion(L"dwrite.dll",
                                                         dwriteVers);
        LOG_FONTINIT(("DelayedInitFontList\n"));
        LOG_FONTINIT(("Start: %s %s\n", nowDate, nowTime));
        LOG_FONTINIT(("Uptime: %9.3f s\n", upTime/1000));
        LOG_FONTINIT(("dwrite version: %s\n", 
                      NS_ConvertUTF16toUTF8(dwriteVers).get()));
        elapsedTime = (t3.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
        LOG_FONTINIT((
          "Total time in DelayedInitFontList:    %9.3f ms (families: %d, %s)\n",
          elapsedTime, systemFonts->GetFontFamilyCount(),
          (mGDIFontTableAccess ? "gdi table access" : "dwrite table access")));
        elapsedTime = (t2.QuadPart - t1.QuadPart) * 1000.0 / frequency.QuadPart;
        LOG_FONTINIT((" --- GetSystemFontCollection:  %9.3f ms\n", elapsedTime));
        elapsedTime = (t3.QuadPart - t2.QuadPart) * 1000.0 / frequency.QuadPart;
        LOG_FONTINIT((" --- iterate over families:    %9.3f ms\n", elapsedTime));
    }
#endif

    return NS_OK;
}

static void
RemoveCharsetFromFontSubstitute(nsAString &aName)
{
    PRInt32 comma = aName.FindChar(PRUnichar(','));
    if (comma >= 0)
        aName.Truncate(comma);
}

#define MAX_VALUE_NAME 512
#define MAX_VALUE_DATA 512

nsresult
gfxDWriteFontList::GetFontSubstitutes()
{
    HKEY hKey;
    DWORD i, rv, lenAlias, lenActual, valueType;
    WCHAR aliasName[MAX_VALUE_NAME];
    WCHAR actualName[MAX_VALUE_DATA];

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, 
          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
          0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return NS_ERROR_FAILURE;
    }

    for (i = 0, rv = ERROR_SUCCESS; rv != ERROR_NO_MORE_ITEMS; i++) {
        aliasName[0] = 0;
        lenAlias = NS_ARRAY_LENGTH(aliasName);
        actualName[0] = 0;
        lenActual = sizeof(actualName);
        rv = RegEnumValueW(hKey, i, aliasName, &lenAlias, NULL, &valueType, 
                (LPBYTE)actualName, &lenActual);

        if (rv != ERROR_SUCCESS || valueType != REG_SZ || lenAlias == 0) {
            continue;
        }

        if (aliasName[0] == WCHAR('@')) {
            continue;
        }

        nsAutoString substituteName((PRUnichar*) aliasName);
        nsAutoString actualFontName((PRUnichar*) actualName);
        RemoveCharsetFromFontSubstitute(substituteName);
        BuildKeyNameFromFontName(substituteName);
        RemoveCharsetFromFontSubstitute(actualFontName);
        BuildKeyNameFromFontName(actualFontName);
        gfxFontFamily *ff;
        if (!actualFontName.IsEmpty() && 
            (ff = mFontFamilies.GetWeak(actualFontName))) {
            mFontSubstitutes.Put(substituteName, ff);
        } else {
            mNonExistingFonts.AppendElement(substituteName);
        }
    }
    return NS_OK;
}

PRBool
gfxDWriteFontList::GetStandardFamilyName(const nsAString& aFontName,
                                         nsAString& aFamilyName)
{
    gfxFontFamily *family = FindFamily(aFontName);
    if (family) {
        family->LocalizedName(aFamilyName);
        return PR_TRUE;
    }

    return PR_FALSE;
}

gfxFontFamily* gfxDWriteFontList::FindFamily(const nsAString& aFamily)
{
    if (!mInitialized) {
        mInitialized = PR_TRUE;
        DelayedInitFontList();
    }

    return gfxPlatformFontList::FindFamily(aFamily);
}

PRBool 
gfxDWriteFontList::ResolveFontName(const nsAString& aFontName,
                                   nsAString& aResolvedFontName)
{
    if (!mInitialized) {
        mInitialized = PR_TRUE;
        DelayedInitFontList();
    }

    nsAutoString keyName(aFontName);
    BuildKeyNameFromFontName(keyName);

    nsRefPtr<gfxFontFamily> ff;
    if (mFontSubstitutes.Get(keyName, &ff)) {
        aResolvedFontName = ff->Name();
        return PR_TRUE;
    }

    if (mNonExistingFonts.Contains(keyName)) {
        return PR_FALSE;
    }

    return gfxPlatformFontList::ResolveFontName(aFontName, aResolvedFontName);
}
