#include "PdfiumLoader.h"
#include <mutex>
#include <vector>

#if defined( _WIN32 )
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#else
#	include <dlfcn.h>
#endif

namespace pdfv
{
namespace
{
#if defined( _WIN32 )
std::wstring Utf8ToWide( const std::string& s )
{
	if( s.empty() )
		return {};
	int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
	std::wstring w( n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), &w[ 0 ], n );
	return w;
}
std::string WideToUtf8( const std::wstring& w )
{
	if( w.empty() )
		return {};
	int n = WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr );
	std::string s( n, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), &s[ 0 ], n, nullptr, nullptr );
	return s;
}
#endif

void* OpenLibrary( const std::string& path )
{
#if defined( _WIN32 )
	// Full path + "search only the DLL's own folder for its dependencies".
	return (void*)LoadLibraryExW( Utf8ToWide( path ).c_str(), nullptr,
	                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 );
#else
	return dlopen( path.c_str(), RTLD_NOW | RTLD_LOCAL );
#endif
}

void* GetSymbol( void* lib, const char* name )
{
#if defined( _WIN32 )
	return (void*)GetProcAddress( (HMODULE)lib, name );
#else
	return dlsym( lib, name );
#endif
}

std::vector< std::string > CandidatePaths()
{
	const std::string dir = GetPluginDirectory();
#if defined( _WIN32 )
	return { dir + "PDFViewer_pdfium.dll", dir + "PDFViewer\\PDFViewer_pdfium.dll" };
#elif defined( __APPLE__ )
	// dir = .../PDFViewer.bundle/Contents/MacOS/
	return { dir + "../Frameworks/libpdfium.dylib", dir + "libpdfium.dylib" };
#else
	return { dir + "libpdfium.so" };
#endif
}

template< typename T >
bool Load( void* lib, T& fn, const char* name, std::string& missing )
{
	fn = reinterpret_cast< T >( GetSymbol( lib, name ) );
	if( !fn )
		missing += std::string( missing.empty() ? "" : ", " ) + name;
	return fn != nullptr;
}

std::once_flag g_once;
PdfiumApi g_api;
bool g_ok = false;
std::string g_error;

void LoadOnce()
{
	void* lib = nullptr;
	std::string tried;
	for( const auto& p : CandidatePaths() )
	{
		lib = OpenLibrary( p );
		if( lib )
			break;
		tried += "\n  " + p;
	}
	if( !lib )
	{
		g_error = "PDFium library not found. Tried:" + tried;
		return;
	}

	std::string missing;
	Load( lib, g_api.InitLibraryWithConfig, "FPDF_InitLibraryWithConfig", missing );
	Load( lib, g_api.GetLastError, "FPDF_GetLastError", missing );
	Load( lib, g_api.LoadMemDocument64, "FPDF_LoadMemDocument64", missing );
	Load( lib, g_api.CloseDocument, "FPDF_CloseDocument", missing );
	Load( lib, g_api.GetPageCount, "FPDF_GetPageCount", missing );
	Load( lib, g_api.GetPageSizeByIndex, "FPDF_GetPageSizeByIndex", missing );
	Load( lib, g_api.LoadPage, "FPDF_LoadPage", missing );
	Load( lib, g_api.ClosePage, "FPDF_ClosePage", missing );
	Load( lib, g_api.BitmapCreateEx, "FPDFBitmap_CreateEx", missing );
	Load( lib, g_api.BitmapFillRect, "FPDFBitmap_FillRect", missing );
	Load( lib, g_api.BitmapDestroy, "FPDFBitmap_Destroy", missing );
	Load( lib, g_api.RenderPageBitmap, "FPDF_RenderPageBitmap", missing );
	if( !missing.empty() )
	{
		g_error = "PDFium library is missing functions: " + missing;
		return;
	}

	FPDF_LIBRARY_CONFIG config{};
	config.version = 2;
	g_api.InitLibraryWithConfig( &config );
	// Never unloaded / destroyed: hosts load & unload plugins freely and
	// tearing PDFium down while another instance still uses it is not worth the risk.
	g_ok = true;
}

}// namespace

std::string GetPluginDirectory()
{
#if defined( _WIN32 )
	HMODULE self = nullptr;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    (LPCWSTR)&GetPluginDirectory, &self );
	std::wstring buf( 32768, L'\0' );
	DWORD n = GetModuleFileNameW( self, &buf[ 0 ], (DWORD)buf.size() );
	buf.resize( n );
	std::string path = WideToUtf8( buf );
	size_t slash     = path.find_last_of( "\\/" );
	return slash == std::string::npos ? std::string() : path.substr( 0, slash + 1 );
#else
	Dl_info info{};
	if( !dladdr( (void*)&GetPluginDirectory, &info ) || !info.dli_fname )
		return {};
	std::string path = info.dli_fname;
	size_t slash     = path.find_last_of( '/' );
	return slash == std::string::npos ? std::string() : path.substr( 0, slash + 1 );
#endif
}

const PdfiumApi* GetPdfium( std::string* errorOut )
{
	std::call_once( g_once, LoadOnce );
	if( !g_ok && errorOut )
		*errorOut = g_error;
	return g_ok ? &g_api : nullptr;
}

}// namespace pdfv
