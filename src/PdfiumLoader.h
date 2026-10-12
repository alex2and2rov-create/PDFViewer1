#pragma once
// Runtime loader for the PDFium shared library.
//
// The plugin does NOT link against pdfium at build time. Instead it loads its
// own uniquely-named copy (PDFViewer_pdfium.dll / libpdfium.dylib inside the
// bundle) by full path. This avoids the Windows "one DLL per file name per
// process" problem described in the FFGL SDK docs: if Resolume or another
// plugin ever loads a pdfium.dll of its own, ours is still the one we use.

#include <fpdfview.h>
#include <string>

namespace pdfv
{
struct PdfiumApi
{
	decltype( &FPDF_InitLibraryWithConfig ) InitLibraryWithConfig = nullptr;
	decltype( &FPDF_GetLastError ) GetLastError                   = nullptr;
	decltype( &FPDF_LoadMemDocument64 ) LoadMemDocument64         = nullptr;
	decltype( &FPDF_CloseDocument ) CloseDocument                 = nullptr;
	decltype( &FPDF_GetPageCount ) GetPageCount                   = nullptr;
	decltype( &FPDF_GetPageSizeByIndex ) GetPageSizeByIndex       = nullptr;
	decltype( &FPDF_LoadPage ) LoadPage                           = nullptr;
	decltype( &FPDF_ClosePage ) ClosePage                         = nullptr;
	decltype( &FPDFBitmap_CreateEx ) BitmapCreateEx               = nullptr;
	decltype( &FPDFBitmap_FillRect ) BitmapFillRect               = nullptr;
	decltype( &FPDFBitmap_Destroy ) BitmapDestroy                 = nullptr;
	decltype( &FPDF_RenderPageBitmap ) RenderPageBitmap           = nullptr;
};

// Loads and initialises PDFium once per process. Thread-safe.
// Returns nullptr on failure; errorOut then explains why.
const PdfiumApi* GetPdfium( std::string* errorOut = nullptr );

// Directory (with trailing separator) that contains this plugin's binary.
std::string GetPluginDirectory();

}// namespace pdfv
